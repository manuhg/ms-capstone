#include <iostream>
#include <string>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <map>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <pqxx/pqxx>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <random>
#include <poppler/cpp/poppler-document.h>
#include <poppler/cpp/poppler-page.h>
#include <thread>
#include <mutex>
#include <cstdlib>
#include <vector>
#include <regex>
#include <nlohmann/json.hpp>
#include <functional>
#include<unordered_set>
#include<set>
#include <openssl/md5.h> // Include for MD5 hashing
#include "lib_tldr.h"

// Helper function to extract content from XML tags
std::string extract_xml_content(const std::string &xml) {
    size_t start = xml.find('>');
    if (start == std::string::npos) return "";

    size_t end = xml.rfind("<");
    if (end == std::string::npos || end <= start) return "";

    return xml.substr(start + 1, end - start - 1);
}

#include "llama.h"
#include "llm/llm-wrapper.h"
#include "lib_tldr/constants.h"

using json = nlohmann::json;

// Global database instance
std::unique_ptr<tldr::Database> g_db;

#if !USE_POSTGRES
// Global mutex for thread synchronization
// std::mutex g_mutex;
#endif
PdfMetadata getPdfMetadata(const std::string &filename) {
    PdfMetadata metadata;
    std::string expanded_path = translatePath(filename);
    auto doc = std::unique_ptr<poppler::document>(poppler::document::load_from_file(expanded_path));

    if (!doc) {
        std::cerr << "Error opening PDF file at path: " << expanded_path << std::endl;
        metadata.pageCount = -1; // Indicate error with page count -1
        return metadata;
    }

    // Get page count
    metadata.pageCount = doc->pages();

    // Extract metadata fields
    poppler::ustring metadata_ustr = doc->metadata();
    if (!metadata_ustr.empty()) {
        std::string metadata_str = metadata_ustr.to_latin1();
        std::istringstream meta_stream(metadata_str);
        std::string line;

        while (std::getline(meta_stream, line)) {
            if (line.find("<dc:title>") != std::string::npos) {
                metadata.title = extract_xml_content(line);
            } else if (line.find("<dc:creator>") != std::string::npos) {
                metadata.author = extract_xml_content(line);
            } else if (line.find("<dc:subject>") != std::string::npos) {
                metadata.subject = extract_xml_content(line);
            } else if (line.find("<dc:description>") != std::string::npos) {
                // Use description for keywords if needed
                if (metadata.keywords.empty()) {
                    metadata.keywords = extract_xml_content(line);
                }
            } else if (line.find("<pdf:Producer>") != std::string::npos) {
                metadata.producer = extract_xml_content(line);
            } else if (line.find("<pdf:Creator>") != std::string::npos) {
                metadata.creator = extract_xml_content(line);
            }
        }
    }

    return metadata;
}

std::string translatePath(const std::string &path) {
    std::string result = path;

    // Expand tilde (~)
    if (!result.empty() && result[0] == '~') {
        const char *home = std::getenv("HOME");
        if (home) {
            result.replace(0, 1, home);
        }
    }

    // Expand $VARS
    static const std::regex env_pattern(R"(\$([A-Za-z_]\w*))");
    std::string expanded;
    std::sregex_iterator it(result.begin(), result.end(), env_pattern);
    std::sregex_iterator end;

    size_t last_pos = 0;
    for (; it != end; ++it) {
        const std::smatch &match = *it;
        expanded.append(result, last_pos, match.position() - last_pos);
        const char *val = std::getenv(match[1].str().c_str());
        expanded.append(val ? val : "");
        last_pos = match.position() + match.length();
    }
    expanded.append(result, last_pos);

    return expanded;
}

int calc_batch_chars(const std::vector<std::string_view> &batch) {
    int total_length = 0;
    if (batch.empty()) {
        return 0;
    }
    for (const auto &c: batch) {
        total_length += c.length();
    }
    return total_length;
}

// Extract document data including metadata and page texts from a PDF file
DocumentData extractDocumentDataFromPDF(const std::string &filename) {
    DocumentData docData;
    std::string expanded_path = translatePath(filename);
    auto doc = std::unique_ptr<poppler::document>(poppler::document::load_from_file(expanded_path));

    // Check if the document loaded successfully
    if (!doc) {
        std::cerr << "Error opening PDF file at path: " << expanded_path << std::endl;
        docData.metadata.pageCount = -1; // Indicate error with page count -1
        return docData;
    }

    // Get metadata
    docData.metadata = getPdfMetadata(filename);

    // Pre-allocate space for page texts
    int pageCount = doc->pages();
    docData.pageTexts.reserve(pageCount);

    // Extract text from each page
    for (int i = 0; i < pageCount; ++i) {
        auto page = std::unique_ptr<poppler::page>(doc->create_page(i));
        if (page) {
            std::string page_text;
            poppler::byte_array utf8_data = page->text().to_utf8();

            // Only keep ASCII characters for now
            page_text.reserve(utf8_data.size());
            for (unsigned char c: utf8_data) {
                if (c < 128) {
                    // ASCII range
                    page_text += c;
                }
            }

            docData.pageTexts.push_back(page_text);
        } else {
            docData.pageTexts.push_back(""); // Push empty string for unreadable pages
        }
    }

    return docData;
}

// Kept for backward compatibility
std::string extractTextFromPDF(const std::string &filename) {
    DocumentData docData = extractDocumentDataFromPDF(filename);
    std::string fullText;

    // Concatenate all page texts with delimiters
    for (const auto &pageText: docData.pageTexts) {
        if (!pageText.empty()) {
            fullText += pageText + PAGE_DELIMITER;
        }
    }

    return fullText;
}


// Split document text into chunks with page tracking
void splitTextIntoChunks(DocumentData &docData, size_t max_chunk_size, size_t overlap) {
    // Clear any existing chunks
    docData.chunks.clear();
    docData.chunkPageNums.clear();

    // Concatenate all page texts with delimiters
    std::string fullText;
    std::vector<size_t> pageBoundaries;
    size_t current_pos = 0;

    for (const auto &pageText: docData.pageTexts) {
        fullText += pageText;
        current_pos += pageText.length();
        pageBoundaries.push_back(current_pos);
    }

    const size_t text_len = fullText.length();

    size_t pos = 0;
    size_t currentPage = 0;

    while (pos < text_len) {
        // Calculate end position for this chunk
        size_t chunk_end = std::min(pos + max_chunk_size, text_len);

        // Find which page this chunk starts in
        while (currentPage < pageBoundaries.size() && pos >= pageBoundaries[currentPage]) {
            currentPage++;
        }

        // Add the chunk
        int num_chars = chunk_end - pos;
        docData.chunks.push_back(fullText.substr(pos, num_chars));
        docData.chunkPageNums.push_back(currentPage + 1); // 1-based page numbers

        // Move position for next chunk, accounting for overlap
        pos = num_chars > overlap ? chunk_end - overlap : chunk_end;
    }
}

bool initializeDatabase(const std::string &conninfo) {
    std::cout << "Initializing database..." << std::endl;

    // If no connection string is provided, use the default from constants.h
    const std::string &connection_string = conninfo.empty() ? PG_CONNECTION : conninfo;

    try {
        if (!g_db) {
            // if (USE_POSTGRES) {
            g_db = std::make_unique<tldr::PostgresDatabase>(connection_string);
            // } else {
            // g_db = std::make_unique<tldr::SQLiteDatabase>(translatePath(DB_PATH));
            // }

            if (!g_db->initialize()) {
                std::cerr << "Failed to initialize database" << std::endl;
                g_db.reset();
                return false;
            }
        }
        return true;
    } catch (const std::exception &e) {
        std::cerr << "Database initialization error: " << e.what() << std::endl;
        g_db.reset();
        return false;
    }
}

int64_t saveEmbeddingsToDb(const std::vector<std::string_view> &chunks,
                           const std::vector<std::vector<float> > &embeddings,
                           const std::vector<uint64_t> &embeddings_hash,
                           const std::vector<int> &chunkPageNums,
                           const std::string &fileHash) {
    if (!g_db) {
        std::cerr << "Database not initialized" << std::endl;
        return -1;
    }

    // Convert embeddings to JSON format
    json embeddings_json;
    embeddings_json["embeddings"] = json::array();
    for (const auto &emb: embeddings) {
        embeddings_json["embeddings"].push_back(emb);
    }

    if (embeddings_json["embeddings"].empty()) {
        std::cerr << "No embeddings to save." << std::endl;
        return -1;
    }

    // Pass the computed embeddings_hash to the database
    return g_db->saveEmbeddings(chunks, embeddings_json, embeddings_hash, chunkPageNums, fileHash);
}

// Save or update document metadata in the database
bool saveOrUpdateDocumentInDB(const std::string &fileHash,
                              const std::string &filePath,
                              const DocumentData &docData) {
    if (!g_db || fileHash.empty() || filePath.empty()) {
        std::cerr << "Error: Database not initialized or invalid file hash/path" << std::endl;
        return false;
    }

    // Extract filename from path
    std::filesystem::path fsPath(filePath);
    std::string fileName = fsPath.filename().string();

    // Use the database interface to save document metadata
    return g_db->saveDocumentMetadata(
        fileHash,
        filePath,
        fileName,
        docData.metadata.title,
        docData.metadata.author,
        docData.metadata.subject,
        docData.metadata.keywords,
        docData.metadata.creator,
        docData.metadata.producer,
        docData.metadata.pageCount
    );
}


// Parse response and extract embeddings
json parseEmbeddingsResponse(const std::string &response_data) {
    try {
        json response = json::parse(response_data);

        // Validate the response structure
        if (!response.contains("data") || !response["data"].is_array()) {
            // std::cerr << "Full response: " << response.dump(2) << std::endl;
            throw std::runtime_error("Invalid response format: missing 'data' array");
        }

        // Create a new JSON object to store embeddings
        json embeddings_json;
        embeddings_json["embeddings"] = json::array();

        // Extract embeddings from each data item
        for (const auto &item: response["data"]) {
            if (item.contains("embedding") && item["embedding"].is_array()) {
                embeddings_json["embeddings"].push_back(item["embedding"]);
            }
        }

        // Verify we extracted embeddings
        if (embeddings_json["embeddings"].empty()) {
            std::cerr << "No embeddings found in the response" << std::endl;
            throw std::runtime_error("No embeddings found in the response");
        }

        return embeddings_json;
    } catch (const json::parse_error &e) {
        std::cerr << "Raw response data: " << response_data << std::endl;
        throw std::runtime_error(std::string("Failed to parse response: ") + e.what());
    }
}

// Helper: Compute hash for each embedding using MD5 to minimize collisions
#include <openssl/md5.h>
static std::vector<uint64_t> computeEmbeddingHashes(const std::vector<std::vector<float> > &embeddings_list) {
    std::vector<uint64_t> hashes;
    hashes.reserve(embeddings_list.size());
    
    // MD5 produces a 128-bit hash, which we'll convert to a 64-bit hash for compatibility
    unsigned char md5_result[MD5_DIGEST_LENGTH]; // 16 bytes = 128 bits
    
    for (const auto &emb: embeddings_list) {
        // Create a buffer to hold the raw float data
        const size_t buffer_size = emb.size() * sizeof(float);
        unsigned char* buffer = new unsigned char[buffer_size];
        
        // Copy the float data to the buffer
        for (size_t i = 0; i < emb.size(); i++) {
            float val = emb[i];
            // Copy the bytes of the float to the buffer
            memcpy(buffer + (i * sizeof(float)), &val, sizeof(float));
        }
        
        // Compute MD5 hash of the buffer
        MD5(buffer, buffer_size, md5_result);
        
        // Convert the first 8 bytes (64 bits) of the MD5 hash to a uint64_t
        uint64_t hash_value = 0;
        for (int i = 0; i < 8; i++) {
            hash_value |= static_cast<uint64_t>(md5_result[i]) << (i * 8);
        }
        
        // Clean up the buffer
        delete[] buffer;
        
        hashes.push_back(hash_value);
    }
    
    return hashes;
}

// Save embeddings to database with thread safety
int saveEmbeddingsThreadSafe(const std::vector<std::string_view> &batch,
                             const std::vector<std::vector<float> > &batch_embeddings,
                             const std::vector<uint64_t> &embeddings_hash,
                             const std::vector<int> &chunkPageNums,
                             const std::string &fileHash) {
    json embeddings_json;
    embeddings_json["embeddings"] = json::array();
    for (const auto &emb: batch_embeddings) {
        embeddings_json["embeddings"].push_back(emb);
    }

    if (embeddings_json["embeddings"].empty()) {
        std::cerr << "  No embeddings generated for this batch." << std::endl;
        return -1;
    }


#if !USE_POSTGRES
    // std::lock_guard<std::mutex> lock(g_mutex);
#endif
    // Pass the raw embeddings and computed hashes to saveEmbeddingsToDb
    int64_t saved_id = saveEmbeddingsToDb(batch, batch_embeddings, embeddings_hash, chunkPageNums, fileHash);
    if (saved_id < 0) {
        std::cerr << "Failed to save embeddings to database";
        // throw std::runtime_error("Failed to save embeddings to database");
    }
    return saved_id;
}

bool initializeSystem(const std::string& chat_model_path, const std::string& embeddings_model_path) {
    std::cout << "Initialize the system" << std::endl;
    if (!initializeDatabase()) {
        std::cerr << "Failed to initialize database" << std::endl;
        return false;
    }
    
    // Use default paths if not provided
    std::string chat_path = chat_model_path.empty() ? CHAT_MODEL_PATH : chat_model_path;
    std::string embeddings_path = embeddings_model_path.empty() ? EMBEDDINGS_MODEL_PATH : embeddings_model_path;
    
    tldr::initialize_llm_manager_once(chat_path, embeddings_path);
    return true;
}

void closeDatabase() {
    // Reset the global database instance, which will clean up the connection pool
    g_db.reset();
    std::cout << "Database connection closed." << std::endl;
}

void cleanupSystem() {
    // Close the database connection
    closeDatabase();

    // Clean up the LLM manager
    tldr::get_llm_manager().cleanup();

    std::cout << "System cleaned up." << std::endl;
}

// Vector dump functionality is now in vec_dump.h/cpp

// Function declarations moved to the top of the file

// Process a block of batches for embedding generation
void processBatchBlocks(
    size_t thread_id,
    size_t start_batch,
    size_t end_batch,
    const std::vector<std::string> &chunks,
    const std::vector<int> &chunkPageNums,
    const std::string &fileHash,
    size_t batch_size,
    std::vector<std::vector<float> > &all_embeddings,
    std::vector<uint64_t> &all_hashes,
    std::mutex &result_mutex
) {
    // Thread-local vectors to store results
    std::vector<std::vector<float> > local_embeddings;
    std::vector<uint64_t> local_hashes;

    std::cout << "Thread " << thread_id << " started, processing batches from "
            << start_batch << " to " << end_batch << std::endl;

    // Calculate actual chunk indices from batch numbers
    size_t start_chunk = start_batch * batch_size;
    size_t end_chunk = std::min(end_batch * batch_size, chunks.size());

    // Process all chunks assigned to this thread
    for (size_t chunk_idx = start_chunk; chunk_idx < end_chunk; chunk_idx += batch_size) {
        // Calculate the end of this batch
        size_t batch_end = std::min(chunk_idx + batch_size, chunks.size());

        // Create a vector of string_view for the current batch
        std::vector<std::string_view> batch_chunks(
            chunks.begin() + chunk_idx,
            chunks.begin() + batch_end
        );

        // Get the page numbers for this batch
        std::vector<int> batch_page_nums(
            chunkPageNums.begin() + chunk_idx,
            chunkPageNums.begin() + batch_end);

        // std::cout << "Thread " << thread_id << " processing chunks: " << chunk_idx
        //           << "-" << batch_end << std::endl;

        // Get embeddings for this batch
        std::vector<std::vector<float> > batch_emb = tldr::get_llm_manager().get_embeddings(batch_chunks);

        // Compute hashes for these embeddings
        std::vector<uint64_t> batch_hashes = computeEmbeddingHashes(batch_emb);

        // Save to DB
        saveEmbeddingsThreadSafe(batch_chunks, batch_emb, batch_hashes, batch_page_nums, fileHash);

        // Append to thread-local vectors
        local_embeddings.insert(local_embeddings.end(), batch_emb.begin(), batch_emb.end());
        local_hashes.insert(local_hashes.end(), batch_hashes.begin(), batch_hashes.end());
    }

    // Merge results into the global vectors using mutex for thread safety
    std::lock_guard<std::mutex> lock(result_mutex);
    all_embeddings.insert(all_embeddings.end(),
                          local_embeddings.begin(),
                          local_embeddings.end());
    all_hashes.insert(all_hashes.end(),
                      local_hashes.begin(),
                      local_hashes.end());
}

std::pair<std::vector<std::vector<float> >, std::vector<uint64_t> >
obtainEmbeddings(const std::vector<std::string> &chunks,
                 const std::vector<int> &chunkPageNums,
                 const std::string &fileHash,
                 size_t batch_size, size_t num_threads) {
    const size_t total_batches = (chunks.size() + batch_size - 1) / batch_size;
    std::cout << "Processing " << chunks.size() << " chunks in " << total_batches
            << " batches using " << num_threads << " threads\n";

    // We'll collect all embeddings and hashes
    std::vector<std::vector<float> > all_embeddings;
    std::vector<uint64_t> all_hashes;
    all_embeddings.reserve(chunks.size());
    all_hashes.reserve(chunks.size());

    // Mutex for thread synchronization when merging results
    std::mutex result_mutex;

    try {
        // Calculate how many batches each thread should process
        size_t batches_per_thread = (total_batches + num_threads - 1) / num_threads;

        // Vector to hold all thread objects
        std::vector<std::thread> threads;

        // Create and start threads
        for (size_t t = 0; t < num_threads; ++t) {
            size_t start_batch = t * batches_per_thread;
            size_t end_batch = std::min((t + 1) * batches_per_thread, total_batches);

            // Only create a thread if there's work to do
            if (start_batch < total_batches) {
                threads.emplace_back(processBatchBlocks, t, start_batch, end_batch,
                                     std::ref(chunks), std::ref(chunkPageNums), std::ref(fileHash),
                                     batch_size, std::ref(all_embeddings), std::ref(all_hashes),
                                     std::ref(result_mutex));
            }
        }

        // Wait for all threads to complete
        for (auto &thread: threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }

        std::cout << "Completed processing all chunks. Total embeddings: "
                << all_embeddings.size() << ", total hashes: "
                << all_hashes.size() << std::endl;
    } catch (const std::exception &e) {
        std::cerr << "Error processing chunks: " << e.what() << std::endl;
        throw; // Re-throw to allow proper cleanup
    }
    return {all_embeddings, all_hashes};
}

bool addFileToCorpus(const std::string &sourcePath, const std::string &fileHash) {
    std::cout << "Processing file: " << sourcePath << std::endl;
    auto start = std::chrono::high_resolution_clock::now();
    try {
        std::string expanded_path = translatePath(sourcePath);

        // Extract document data and metadata
        DocumentData docData = extractDocumentDataFromPDF(expanded_path);
        if (docData.pageTexts.empty()) {
            std::cerr << "Error: No text extracted from PDF." << std::endl;
            return false;
        }

        // Save or update document metadata in the database
        if (!saveOrUpdateDocumentInDB(fileHash, expanded_path, docData)) {
            std::cerr << "Error: Failed to save document metadata to database" << std::endl;
            return false;
        }

        // Delete any existing embeddings for this file hash
        if (!deleteFileEmbeddingsFromDB(fileHash)) {
            std::cerr << "Warning: Failed to delete existing embeddings for file hash: " << fileHash << std::endl;
            // Continue anyway, as we'll try to add new embeddings
        }

        // Split into chunks with page tracking
        splitTextIntoChunks(docData, MAX_CHUNK_SIZE, CHUNK_N_OVERLAP);

        // Print text length and chunk info
        std::cout << "Extracted " << docData.pageTexts.size() << " pages with "
                << docData.chunks.size() << " chunks" << std::endl;

        // Get embeddings and their hashes, and save them directly in the worker threads
        auto [embeddings, hashes] = obtainEmbeddings(
            docData.chunks, docData.chunkPageNums, fileHash,BATCH_SIZE, EMB_PROC_NUM_THREADS);

        // The embeddings are now saved in the database by obtainEmbeddings
        // We just need to verify that we got the expected number of embeddings
        if (embeddings.size() != docData.chunks.size()) {
            std::cerr << "Error: Mismatch between number of chunks (" << docData.chunks.size()
                    << ") and embeddings (" << embeddings.size() << ")" << std::endl;
            return false;
        }

        // Dump vectors and hashes to file for memory mapping
        if (!tldr::dump_vectors_to_file(expanded_path, embeddings, hashes, fileHash)) {
            // Even if file dump fails, we still have the data in the database
            std::cerr << "Warning: Failed to save vector dump file, but data is saved in database" << std::endl;
        }

        std::cout << "Document added to corpus successfully." << std::endl;
        auto end = std::chrono::high_resolution_clock::now();
        std::cout << " Took " << (end - start).count()/1000000000.0 << "s for file " << sourcePath << std::endl;
        return true;
    } catch (const std::exception &e) {
        std::cerr << "Error processing " << sourcePath << ": " << e.what() << std::endl;
    }
    return false;
}

bool deleteFileEmbeddingsFromDB(const std::string &fileHash) {
    if (g_db) {
        return g_db->deleteEmbeddings(fileHash);
    }
    return false;
}


void findFilesOfTypeRecursively(const std::filesystem::path &path, std::vector<std::string> &files,
                                const std::string &extension) {
    try {
        if (std::filesystem::exists(path)) {
            for (const auto &entry: std::filesystem::recursive_directory_iterator(path)) {
                if (entry.is_regular_file() && entry.path().extension() == extension) {
                    files.push_back(entry.path().string());
                }
            }
        }
    } catch (const std::exception &e) {
        std::cerr << "Error scanning directory " << path << ": " << e.what() << std::endl;
    }
}

// Deprecated: Use findFilesOfTypeRecursively instead
void findPdfFiles(const std::filesystem::path &path, std::vector<std::string> &pdfFiles) {
    // Call the new generic function with ".pdf" extension
    findFilesOfTypeRecursively(path, pdfFiles, ".pdf");
}

std::vector<std::string> collectPdfFiles(const std::string &path) {
    std::vector<std::string> files;
    std::string expanded_path = translatePath(path);

    if (std::filesystem::is_regular_file(expanded_path)) {
        // If it's a PDF file, add it to the list
        if (expanded_path.ends_with(".pdf")) {
            files.push_back(expanded_path);
        } else {
            std::cerr << "Error: Unsupported file type. Only PDF files are supported." << std::endl;
        }
        return files; // Will be empty if not a PDF
    }

    if (std::filesystem::is_directory(expanded_path)) {
        // Find all PDF files recursively using the new generic function
        findFilesOfTypeRecursively(expanded_path, files, ".pdf");

        if (files.empty()) {
            std::cerr << "No PDF files found in " << expanded_path << std::endl;
        }
        return files; // Will be empty if no PDFs found
    }

    std::cerr << "Error: Path is neither a file nor a directory: " << expanded_path << std::endl;
    return {}; // Return empty vector on error
}

bool getFilesToBeEmbedded(const std::string &sourcePath, std::vector<std::string> filesToProcess,
                          std::map<std::string, std::string> fileHashes,
                          std::vector<std::pair<std::string, std::string> > &filesWithHashes, WorkResult &value1) {
    // Determine the search directory for vecdump files
    std::filesystem::path searchPath;

    if (std::filesystem::exists(sourcePath)) {
        if (std::filesystem::is_directory(sourcePath)) {
            // If sourcePath is a directory, use it directly
            searchPath = sourcePath;
        } else if (std::filesystem::is_regular_file(sourcePath)) {
            // If sourcePath is a file, use its parent directory
            searchPath = std::filesystem::path(sourcePath).parent_path();
            std::cout << "Source path is a file, using parent directory: " << searchPath.string() << std::endl;
        }
    } else {
        std::cerr << "Warning: Source path does not exist: " << sourcePath << std::endl;
        // Use sourcePath anyway, in case it's a valid path that just doesn't exist yet
        searchPath = sourcePath;
    }

    // Find all existing vecdump files in the search directory
    std::vector<std::string> existingVecdumps;
    if (std::filesystem::exists(searchPath) && std::filesystem::is_directory(searchPath)) {
        findFilesOfTypeRecursively(searchPath, existingVecdumps, ".vecdump");
        std::cout << "Found " << existingVecdumps.size() << " existing vecdump files in " << searchPath.string() <<
                std::endl;
    }

    // Create a set of existing vecdump hashes for faster lookup
    std::unordered_set<std::string> existingHashes;
    for (const auto &vecdumpPath: existingVecdumps) {
        // Extract the hash from the filename (remove path and extension)
        std::filesystem::path path(vecdumpPath);
        std::string filename = path.filename().string();
        // Remove the .vecdump extension
        std::string hash = filename.substr(0, filename.length() - 8); // Remove ".vecdump"
        existingHashes.insert(hash);
    }

    // Process files and check if their hashes already exist
    for (const auto &file: filesToProcess) {
        auto it = fileHashes.find(file);
        if (it != fileHashes.end()) {
            // Check if this hash already exists in the set of vecdump hashes
            if (existingHashes.find(it->second) != existingHashes.end()) {
                std::cout << "Skipping (vecdump exists) for : " << file << " - " << it->second << std::endl;
            } else {
                filesWithHashes.emplace_back(file, it->second);
                std::cout << "Will process: " << file << " Hash: " << it->second << std::endl;
            }
        } else {
            std::cerr << "Warning: Could not compute hash for file: " << file << std::endl;
        }
    }

    if (filesWithHashes.empty()) {
        std::cout << "All files already have corresponding vecdumps. Nothing to process." << std::endl;
        value1 = WorkResult{false, "", "All files are already processed"};
        return false; // Success, just nothing to do
    }

    std::cout << "Found " << filesWithHashes.size() << " files to process (after filtering existing vecdumps)" <<
            std::endl;
    return true;
}

bool addFilesToCorpusSequential(std::vector<std::pair<std::string, std::string> > filesWithHashes, WorkResult &value1) {
    try {
        for (const auto &[filePath, fileHash]: filesWithHashes) {
            addFileToCorpus(filePath, fileHash);
        }
    } catch (const std::exception &e) {
        std::cerr << "Error in addFilesToCorpusSequence()" << std::endl;
        value1 = WorkResult{true, e.what(), ""};
        return false;
    }
    value1 = WorkResult{false, "", ""};
    return true;
}

bool addFilesToCorpus(std::vector<std::pair<std::string, std::string> > filesWithHashes, WorkResult &value1) {
    // Determine number of threads to use
    const size_t numThreads = std::min(filesWithHashes.size(), static_cast<size_t>(ADD_CORPUS_N_THREADS));

    std::cout << "Using " << numThreads << " threads for processing " << filesWithHashes.size() << " files" <<
            std::endl;
    // Process files in parallel
    std::vector<std::thread> threads;
    std::atomic<bool> has_errors{false};
    std::string last_error;

    // Split work among threads
    const size_t filesPerThread = (filesWithHashes.size() + numThreads - 1) / numThreads;

    for (size_t i = 0; i < numThreads; ++i) {
        const size_t start = i * filesPerThread;
        if (start >= filesWithHashes.size()) break;

        const size_t end = std::min(start + filesPerThread, filesWithHashes.size());

        threads.emplace_back([&filesWithHashes, start, end, &has_errors, &last_error]() {
            try {
                for (size_t j = start; j < end; ++j) {
                    // if (has_errors) break; // Early exit if another thread failed

                    const auto &[filePath, fileHash] = filesWithHashes[j];

                    try {
                        // Process the file
                        addFileToCorpus(filePath, fileHash);

                        // Print progress
                        std::cout << "Processed: " << filePath << std::endl;
                    } catch (const std::exception &e) {
                        has_errors = true;
                        last_error = std::string("Error processing ") + filePath + ": " + e.what();
                        std::cerr << last_error << std::endl;
                    }
                }
            } catch (const std::exception &e) {
                has_errors = true;
                last_error = std::string("Thread error: ") + e.what();
                std::cerr << last_error << std::endl;
            }
        });
    }

    // Wait for all threads to complete
    for (auto &thread: threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }

    if (has_errors) {
        value1 = WorkResult::Error(last_error.empty() ? "Unknown error during processing" : last_error);
        return false;
    }
    return true;
}

WorkResult addCorpus(const std::string &sourcePath) {
    std::string expanded_path = translatePath(sourcePath);
    try {
        WorkResult result;
        // Collect PDF files - using move semantics for efficiency
        std::vector<std::string> pdfFiles = collectPdfFiles(expanded_path);
        if (pdfFiles.empty()) {
            return WorkResult::Error("No PDF files found to process");
        }
        std::cout << "Found " << pdfFiles.size() << " PDF files to process" << std::endl;

        std::map<std::string, std::string> fileHashes;
        if (!computeFileHashes(pdfFiles, fileHashes, result))
            return result;

        std::vector<std::pair<std::string, std::string> > filesToEmbed;
        if (!getFilesToBeEmbedded(expanded_path, pdfFiles, fileHashes, filesToEmbed, result))
            return result;

#if CORPUS_FILE_PROC_TYPE==CORPUS_FILE_PROC_TYPE_PARALLEL
        if (!addFilesToCorpus(filesToEmbed, result))
            return result;
#else
        if (!addFilesToCorpusSequential(filesToEmbed, result))
            return result;
#endif

        return WorkResult{false, "", std::format("Processed {} files", filesToEmbed.size())}; // Success
    } catch (const std::exception &e) {
        return WorkResult::Error(std::string("Error in addCorpus: ") + e.what());
    }
}

void deleteCorpus(const std::string &corpusId) {
    // Implement the function
    std::cout << "DELETE_CORPUS action with corpus_id: " << corpusId << std::endl;
}

void doRag(const std::string &conversationId) {
    // Implement the function
    std::cout << "DO_RAG action with conversation_id: " << conversationId << std::endl;

    try {
        pqxx::connection C("dbname=testdb user=postgres password=secret hostaddr=127.0.0.1 port=5432");
        pqxx::work W(C);

        std::string query =
                "SELECT * FROM conversations WHERE id = " + W.quote(conversationId) + " ORDER BY created_at";
        pqxx::result R = W.exec(query);

        for (auto row: R) {
            std::cout << "ID: " << row["id"].c_str() << ", Created At: " << row["created_at"].c_str() << std::endl;
        }

        W.commit();
    } catch (const std::exception &e) {
        std::cerr << e.what() << std::endl;
    }
}

RagResult queryRag(const std::string &user_query, const std::string &corpus_dir, const std::string &npu_model_path) {
    RagResult result;

    if (!g_db) {
        std::cerr << "Database not initialized" << std::endl;
        return result;
    }

    try {
        // Get embeddings for the user query using LlmManager
        auto query_embeddings = tldr::get_llm_manager().get_embeddings({user_query});

        if (query_embeddings.empty() || query_embeddings[0].empty()) {
            std::cerr << "Failed to get embeddings for the query." << std::endl;
            return result;
        }

        std::cout << "Using NPU-accelerated similarity search..." << std::endl;

        // Use NPU-accelerated similarity search instead of database search
        std::cout << "Using NPU model path: " << npu_model_path << std::endl;
        auto similar_chunks = searchSimilarVectorsNPU(
            query_embeddings[0], // Query vector
            translatePath(corpus_dir), // Vector corpus directory
            K_SIMILAR_CHUNKS_TO_RETRIEVE, // Number of results to return
            npu_model_path // NPU model path
        );

        // Fallback to traditional database search if NPU search returns no results
        if (similar_chunks.empty()) {
            std::cerr << "No results from NPU search, falling back to database search..." << std::endl;
            
            // Get the results from the traditional database search (which now returns ContextChunk objects)
            similar_chunks = g_db->searchSimilarVectors(query_embeddings[0], K_SIMILAR_CHUNKS_TO_RETRIEVE);
            
            // No need to convert anything since searchSimilarVectors now returns ContextChunk objects
            // with document metadata already included
        }

        // Prepare context string for the LLM and store context chunks
        std::string context_str;
        
        // Track unique documents for reference count
        std::set<std::string> referenced_documents;
        
        // Track unique documents referenced in this result
        std::cout<<"\nPreparing LLM context ..."<<std::endl;
        for (const auto &chunk: similar_chunks) {
            // Format the document metadata to include in the prompt
            std::string source_info;
            if (!chunk.title.empty() || !chunk.file_name.empty()) {
                source_info = "Source: ";
                if (!chunk.title.empty()) {
                    source_info += "title:" + chunk.title;
                } else {
                    source_info += "file: "+ chunk.file_name;
                }
                
                if (!chunk.author.empty()) {
                    source_info += " by " + chunk.author;
                }
                
                if (chunk.page_count > 0) {
                    source_info += " (" + std::to_string(chunk.page_count) + " Pages)";
                }
                
                // Include page number information
                if (chunk.page_number > 0) {
                    source_info += ", Page " + std::to_string(chunk.page_number);
                }
                
                source_info += "\n";
                
                // Track unique documents referenced
                std::string doc_id = !chunk.file_path.empty() ? chunk.file_path : chunk.file_name;
                if (!doc_id.empty()) {
                    referenced_documents.insert(doc_id);
                }
                
                // Document is tracked in referenced_documents set
            }
            
            // Add formatted context with metadata to the LLM prompt
            context_str += source_info + chunk.text + "\n\n";
            
            // Add to result chunks (passing all metadata through)
            result.context_chunks.push_back(chunk);
        }
        
        // Set the count of unique documents referenced
        result.referenced_document_count = referenced_documents.size();

        if (context_str.empty()) {
            std::cerr << "No relevant context found in DB!" << std::endl;
            return result;
        }

        // Generate response using LlmManager's chat model
        result.response = tldr::get_llm_manager().get_chat_response(context_str, user_query);
    } catch (const std::exception &e) {
        std::cerr << "RAG Query error: " << e.what() << std::endl;
        result.response = "Error generating response!";
    }

    return result;
}

std::map<uint64_t, float> npuCosineSimSearchWrapper(
    const float *queryVector, const int queryVectorDimensions,
    const int32_t k,
    const char *corpusDir, const char *modelPath) {
    std::cout << "Calling Swift function from C++..." << std::endl;

    // Define the path to the compiled Core ML model and corpus directory
    std::cout << "C++: Using model path: " << modelPath << std::endl;
    std::cout << "C++: Using corpus directory: " << corpusDir << std::endl;

    int32_t resultCount = 0;
    std::map<uint64_t, float> hash_scores;

    // Call the Swift function
    SimilarityResult *results_ptr = retrieve_similar_vectors_from_corpus(
        modelPath,
        corpusDir,
        queryVector,
        queryVectorDimensions,
        k,
        &resultCount
    );
    if (results_ptr && resultCount > 0) {
        for (int32_t i = 0; i < resultCount; i++)
            hash_scores[results_ptr[i].hash] = results_ptr[i].score;
    } else
        std::cerr << "No similar chunks found! " << std::endl;

    if (results_ptr)
        free_similarity_results(results_ptr);

    return hash_scores;
}

// Wrapper function for NPU-accelerated vector similarity search
std::vector<CtxChunkMeta> searchSimilarVectorsNPU(
    const std::vector<float> &query_vector, const std::string &corpus_dir, int k, const std::string &npu_model_path) {
    std::vector<CtxChunkMeta> similar_chunks;

    // We'll collect the hashes from the results and only then query the database
    // This is more efficient than loading all embeddings upfront
    if (query_vector.size() != EMBEDDING_SIZE_INT) {
        throw std::runtime_error(std::format(
            "Query vector size does not match the pre-defined embedding size! Expected {}, got {}",
            EMBEDDING_SIZE, query_vector.size()));
    }

    try {
        // Get hash scores from NPU-accelerated search
        std::map<uint64_t, float> hash_scores = npuCosineSimSearchWrapper(
            query_vector.data(),
            query_vector.size(),
            k,
            corpus_dir.c_str(),
            npu_model_path.c_str()
        );

        // Print the hash values returned by the NPU search
        std::cout << "NPU search returned the following hashes:" << std::endl;
        for (const auto &[hash, score]: hash_scores) {
            std::cout << "Hash: " << hash << ", Score: " << score << std::endl;
        }

        // Extract hashes for database lookup
        std::vector<uint64_t> hashes_to_lookup;
        hashes_to_lookup.reserve(hash_scores.size());
        for (const auto &[hash, _]: hash_scores) {
            hashes_to_lookup.push_back(hash);
        }

        // Query the database for the text chunks corresponding to these hashes
        std::map<uint64_t, CtxChunkMeta> hash_to_metadata;
        if (g_db) {
            hash_to_metadata = g_db->getChunksByHashes(hashes_to_lookup);
        }

        // Process the results
        for (const auto &[hash, score]: hash_scores) {
            auto it = hash_to_metadata.find(hash);
            if (it != hash_to_metadata.end()) {
                // We already have a CtxChunkMeta, just update the similarity score
                CtxChunkMeta chunk = it->second;
                chunk.similarity = score; // Update with the score from our calculation
                
                similar_chunks.push_back(chunk);
                std::cout << "Found match for hash: " << hash << std::endl;
            } else {
                // If text not found, use hash as identifier
                std::cerr << "HASH_NOT_FOUND-" << hash << std::endl;
            }
        }
    } catch (const std::exception &e) {
        std::cerr << "Error in NPU similarity search: " << e.what() << std::endl;
    }

    return similar_chunks;
}

// Test vector cache dump and read functionality
bool test_vector_cache() {
    std::cout << "=== Testing Vector Cache Dump and Read Functionality ===" << std::endl;

    // Create sample embeddings and hashes
    std::vector<std::vector<float> > test_embeddings;
    std::vector<uint64_t> test_hashes;

    // Create 5 test embeddings with 16 dimensions each
    const size_t num_embeddings = 5;
    const size_t dimensions = 16;

    std::cout << "Creating " << num_embeddings << " test embeddings with "
            << dimensions << " dimensions each" << std::endl;

    // Initialize with deterministic values for testing
    for (size_t i = 0; i < num_embeddings; i++) {
        std::vector<float> embedding(dimensions);
        for (size_t j = 0; j < dimensions; j++) {
            embedding[j] = static_cast<float>((i + 1) * 0.1f + j * 0.01f); // Deterministic pattern
        }
        test_embeddings.push_back(std::move(embedding));

        // Create corresponding hash
        test_hashes.push_back(1000000 + i * 10000); // Simple deterministic hash for testing
    }

    // Test file path
    std::string test_file = "vector_cache_test.bin";

    // Step 1: Dump the test data
    std::cout << "\nStep 1: Dumping test embeddings to " << test_file << std::endl;
    if (!tldr::dump_vectors_to_file(test_file, test_embeddings, test_hashes, "test_hash")) {
        std::cerr << "Error: Failed to dump test embeddings" << std::endl;
        return false;
    }

    // Step 2: Read the dumped file
    std::cout << "\nStep 2: Reading the vector dump file" << std::endl;
    auto mapped_data = tldr::read_vector_dump_file(test_file);
    if (!mapped_data) {
        std::cerr << "Error: Failed to read the vector dump file" << std::endl;
        return false;
    }

    // Step 3: Print info about the file
    print_vector_dump_info(mapped_data.get(), test_file, false);

    // Step 4: Verify contents
    std::cout << "\nStep 4: Verifying file contents" << std::endl;

    // Verify header
    bool header_verified =
            (mapped_data->header->num_entries == num_embeddings) &&
            (mapped_data->header->hash_size_bytes == sizeof(size_t)) &&
            (mapped_data->header->vector_dimensions == dimensions);

    std::cout << "Header verification: " << (header_verified ? "PASSED" : "FAILED") << std::endl;

    // Verify contents by checking values at index 1 (second element)
    bool data_verified = true;
    size_t test_idx = 1;

    if (test_idx < mapped_data->header->num_entries) {
        std::cout << "\nVerifying element at index " << test_idx << ":" << std::endl;

        // Original hash and the one read from file
        size_t original_hash = test_hashes[test_idx];
        size_t read_hash = mapped_data->hashes[test_idx];

        std::cout << "Hash verification: Original = " << original_hash
                << ", Read = " << read_hash
                << " -> " << (original_hash == read_hash ? "MATCH" : "MISMATCH") << std::endl;

        // Check a few dimensions of the embedding vector
        const float *read_vector = mapped_data->vectors + (test_idx * mapped_data->header->vector_dimensions);

        std::cout << "Vector verification (first 5 dimensions):" << std::endl;
        bool vector_matches = true;
        for (size_t i = 0; i < 5 && i < mapped_data->header->vector_dimensions; i++) {
            float original_val = test_embeddings[test_idx][i];
            float read_val = read_vector[i];
            bool matches = (std::abs(original_val - read_val) < 0.000001f); // Floating point comparison with epsilon

            std::cout << "  Dim " << i << ": Original = " << original_val
                    << ", Read = " << read_val
                    << " -> " << (matches ? "MATCH" : "MISMATCH") << std::endl;

            if (!matches) vector_matches = false;
        }

        data_verified = (original_hash == read_hash) && vector_matches;
    }

    // Print final result
    std::cout << "\nTest result: " << (header_verified && data_verified ? "PASSED" : "FAILED") << std::endl;

    return header_verified && data_verified;
}

/*void command_loop() {
    std::string input;
    std::map<std::string, std::function<void(const std::string &)> > actions = {
        {"do-rag", doRag},
        {"add-corpus", addCorpus},
        {"delete-corpus", deleteCorpus},
        {"query", [](const std::string &query) { queryRag(query); }},
        {
            "read-vectors", [](const std::string &path) {
                auto data = tldr::read_vector_dump_file(path);
                if (data) {
                    tldr::print_vector_dump_info(data.get(), path, true);
                } else {
                    std::cerr << "Failed to read vector file: " << path << std::endl;
                }
            }
        },
        {"test-vectors", [](const std::string &) { tldr::test_vector_cache(); }}
    };

    while (true) {
        std::cout << "Enter command: ";
        std::getline(std::cin, input);
        std::istringstream iss(input);
        std::string command, argument;
        iss >> command >> argument;

        auto it = actions.find(command);
        if (it != actions.end()) {
            it->second(argument);
        } else if (command == "exit") {
            break;
        } else {
            std::cout << "Unknown command: " << command << std::endl;
        }
    }
}*/

std::string printRagResult(const RagResult &result) {
    std::stringstream formatted_result;
    
    // Add the LLM response
    formatted_result << "=== LLM Response ===\n\n" << result.response << "\n\n";
    
    // Add information about the number of contexts used
    formatted_result << "=== Context Information ===\n";
    formatted_result << "Referenced " << result.referenced_document_count << " document(s) with " 
                     << result.context_chunks.size() << " context chunk(s)\n\n";
    
    // Add details for each context chunk
    formatted_result << "=== Context Details ===\n\n";
    
    int chunk_number = 1;
    for (const auto& chunk : result.context_chunks) {
        // Create a header for each chunk
        formatted_result << "--- Chunk " << chunk_number++ << " ---\n";
        
        // Document metadata
        formatted_result << "Source: ";
        if (!chunk.title.empty()) {
            formatted_result << "\"" << chunk.title << "\"";
        } else if (!chunk.file_name.empty()) {
            formatted_result << chunk.file_name;
        } else {
            formatted_result << "[Unknown Source]";
        }
        
        if (!chunk.author.empty()) {
            formatted_result << " by " << chunk.author;
        }
        
        if (chunk.page_count > 0) {
            formatted_result << " (" << chunk.page_count << " pages)";
        }
        
        if (chunk.page_number > 0) {
            formatted_result << ", page " << chunk.page_number;
        }
        
        // Add similarity score
        formatted_result << "\nSimilarity: " << std::fixed << std::setprecision(4) << chunk.similarity;
        
        // Add the actual text content
        formatted_result << "\n\nContent:\n" << chunk.text << "\n\n";
    }
    
    return formatted_result.str();
}
