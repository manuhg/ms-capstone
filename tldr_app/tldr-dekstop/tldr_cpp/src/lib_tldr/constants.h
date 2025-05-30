#ifndef TLDR_CPP_CONSTANTS_H
#define TLDR_CPP_CONSTANTS_H
#include <nlohmann/json.hpp>
#include <vector>
#include "definitions.h"
// Text processing constants
#define AVG_WORDS_PER_SENTENCE 6
#define AVG_CHARS_PER_WORD 5
#define CHUNK_N_SENTENCES 10
#define CHUNK_N_OVERLAP 40 // overlap in characters at start and end
#define CHUNK_N_CHARS ((CHUNK_N_SENTENCES * AVG_WORDS_PER_SENTENCE * AVG_CHARS_PER_WORD)+(CHUNK_N_OVERLAP*2))
#define MAX_CHARS_PER_BATCH 512
#define MAX_CHUNK_SIZE (MAX_CHARS_PER_BATCH-(CHUNK_N_OVERLAP*2))
#define BATCH_SIZE 10

#define DB_HASH_PRESENT_UPSERT 1
#define DB_HASH_PRESENT_DO_NOTHING 2
#define DB_HASH_PRESENT_ACTION DB_HASH_PRESENT_DO_NOTHING

#define EMB_PROC_NUM_THREADS 2
#define ADD_CORPUS_N_THREADS (3)  // Maximum number of threads for processing PDFs in parallel
#define DB_CONN_POOL_SIZE 2

// LLM context pool constants
// Chat model context pool sizes
#define CHAT_MIN_CONTEXTS 1
#define CHAT_MAX_CONTEXTS 2
// Embedding model context pool sizes - can have more contexts since embedding operations are faster
#define EMBEDDING_MIN_CONTEXTS (4)
#define EMBEDDING_MAX_CONTEXTS (2+4)

#define CORPUS_FILE_PROC_TYPE_PARALLEL 1
#define CORPUS_FILE_PROC_TYPE_SEQUENTIAL 2
#define CORPUS_FILE_PROC_TYPE CORPUS_FILE_PROC_TYPE_SEQUENTIAL

// Directory name for storing vector cache files
constexpr const char* VECDUMP_DIR = "_vecdumps";
// Database constants
#define USE_POSTGRES true  // Set to true to use PostgreSQL, false for SQLite
#define DB_PATH "~/proj_tldr/datastore/embeddings.db"
#define PG_CONNECTION "dbname=tldr user=postgres password=postgres host=localhost port=5432"

// HTTP request constants
#define EMBEDDINGS_URL "http://localhost:8084/embeddings"
#define CHAT_URL "http://localhost:8088/v1/chat/completions"

#define CHAT_MODEL_PATH "/Users/manu/llm-weights/Llama-3.2-1B-Instruct-Q3_K_L-lms.gguf"
#define EMBEDDINGS_MODEL_PATH "/Users/manu/llm-weights/embedding/all-MiniLM-L6-v2-Q8_0.gguf"

#define CONNECT_TIMEOUT_SECONDS 5
#define REQUEST_TIMEOUT_SECONDS 30
#define MAX_RETRIES 1
#define RETRY_DELAY_MS 1000
#define EMBEDDING_SIZE "384" // keep it string so that it can be inserted into create table stmt
#define EMBEDDING_SIZE_INT 384 // keep it string so that it can be inserted into create table stmt
#define K_SIMILAR_CHUNKS_TO_RETRIEVE 3


NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(embeddings_request, input)
#endif //TLDR_CPP_CONSTANTS_H
