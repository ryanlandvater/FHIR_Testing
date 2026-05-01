# FastFHIR Canonical API Reference
**Extracted from official FastFHIR documentation and validated test suite**

---

## Repository Benchmark Policy (Reviewer Notes)

This repository uses FastFHIR generated `PatientData` and nested `*Data` types as the canonical C++ in-memory ground truth for benchmark inputs.

How this is applied:

- Build one canonical `PatientData` fixture in memory.
- Feed that same object into every benchmark arm.
- JSON/FHIR arm serializes from `PatientData` to JSON text.
- FastFHIR arm serializes from `PatientData` using assignment operators and generated field keys.

Why this matters:

- Ensures all arms start from the same in-memory data.
- Reduces benchmark bias from divergent fixture construction.
- Improves auditability for reviewers by making data provenance explicit.

Note:

- FastFHIR also supports direct struct serialization pathways, but assignment-operator serialization is intentionally used in the benchmark path for cross-system parity.

---

## 1. Loading FFHR Files — Memory::createFromFile vs. Memory::create

### File-Backed Arena (Persistent Storage)
```cpp
#include <FastFHIR.hpp>

// Map the arena straight to a file on disk.
// Every write goes directly into the OS page cache — no separate write() call needed.
// The file is created (or reopened) and grown to the requested capacity.
auto mem = FastFHIR::Memory::createFromFile("patient.ffhr");
```

**From test_readme.cpp — Example 2 (Open and read a .ffhr file):**
```cpp
// Mount the existing archive and traverse directly via Parser::root()
auto mem = FastFHIR::Memory::createFromFile(PATIENT_FFHR, 64 * 1024 * 1024);
auto parser = FastFHIR::Parser(mem);
auto root = parser.root();
REQUIRE(root, "root node is null — is patient.ffhr sealed?");
```

### Anonymous RAM Arena (In-Process Only)
```cpp
#include <FastFHIR.hpp>

// Reserve a 256 MB sparse virtual-address window backed by anonymous RAM.
// Physical pages are only committed by the OS as you actually write to them.
auto mem = FastFHIR::Memory::create();

// Or with explicit capacity:
auto mem = FastFHIR::Memory::create(256 * 1024 * 1024);  // 256 MB
```

### Parse from Raw Bytes (Read-Only)
```cpp
#include <FastFHIR.hpp>
#include <cstdio>
#include <vector>

static std::vector<uint8_t> open_read_only_file(const char* path) {
    std::FILE* fp = std::fopen(path, "rb");
    if (!fp) throw std::runtime_error("failed to open .ffhr file");

    if (std::fseek(fp, 0, SEEK_END) != 0) {
        std::fclose(fp);
        throw std::runtime_error("failed to seek file end");
    }

    long file_size = std::ftell(fp);
    if (file_size <= 0) {
        std::fclose(fp);
        throw std::runtime_error("empty or unreadable file");
    }
    std::rewind(fp);

    std::vector<uint8_t> raw_bytes(static_cast<size_t>(file_size));
    size_t bytes_read = std::fread(raw_bytes.data(), 1, raw_bytes.size(), fp);
    std::fclose(fp);

    if (bytes_read != raw_bytes.size())
        throw std::runtime_error("short read while loading .ffhr file");

    return raw_bytes;
}

// Bind the parser — validates the header immediately, zero heap allocations.
auto raw_bytes = open_read_only_file("patient.ffhr");
FastFHIR::Parser parser(raw_bytes.data(), raw_bytes.size());
auto root = parser.root();
```

---

## 2. Using FastFHIR::Parser

### Basic Read Pattern
```cpp
auto mem = FastFHIR::Memory::createFromFile("patient.ffhr", 64 * 1024 * 1024);
auto parser = FastFHIR::Parser(mem);
auto root = parser.root();

// Typed resource checks (no direct RECOVERY_TAG usage required)
bool parser_says_patient = parser.is_root<FastFHIR::RESOURCETYPE::PATIENT>();
bool root_is_patient     = root.is<FastFHIR::RESOURCETYPE::PATIENT>();
if (!parser_says_patient || !root_is_patient)
    throw std::runtime_error("Expected Patient root resource");
```

### Accessing Scalar Fields
```cpp
// Scalars coerce directly to C++ types — zero heap allocations
std::string_view id     = root[FastFHIR::Fields::PATIENT::ID];           // std::string_view
std::string_view gender = root[FastFHIR::Fields::PATIENT::GENDER];       // std::string_view
bool             active = root[FastFHIR::Fields::PATIENT::ACTIVE].as<bool>(); // bool
std::string_view dob    = root[FastFHIR::Fields::PATIENT::BIRTH_DATE];   // std::string_view
```

### Walking Arrays
```cpp
// Walk structured arrays
for (auto& name_node : root[FastFHIR::Fields::PATIENT::NAME].entries()) {
    std::string_view family = name_node[FastFHIR::Fields::HUMANNAME::FAMILY];
    for (auto& g : name_node[FastFHIR::Fields::HUMANNAME::GIVEN].entries())
        std::cout << g.as<std::string_view>() << " ";
    std::cout << family << "\n";
}
```

### Eager Materialization to Struct
```cpp
// Eagerly materialize into a generated C++ struct (strict schema validation)
PatientData patient_data = root;

// Same API works for polymorphic resource slots (e.g. Bundle.entry.resource):
// if (resource_node.is<FastFHIR::RESOURCETYPE::OBSERVATION>()) { ... }
```

### Reading Fields with Presence Checking
```cpp
// Read scalar fields only if present.
if (auto id_node = root[FastFHIR::Fields::PATIENT::ID])
    std::cout << "id=" << std::string_view(id_node) << "\n";
if (auto active_node = root[FastFHIR::Fields::PATIENT::ACTIVE])
    std::cout << "active=" << std::boolalpha << active_node.as<bool>() << "\n";
if (auto gender_node = root[FastFHIR::Fields::PATIENT::GENDER])
    std::cout << "gender=" << std::string_view(gender_node) << "\n";

// Walk arrays only if the parent field exists.
if (auto name_array = root[FastFHIR::Fields::PATIENT::NAME]) {
    for (auto& name_node : name_array.entries()) {
        if (auto family = name_node[FastFHIR::Fields::HUMANNAME::FAMILY])
            std::cout << "family=" << std::string_view(family) << "\n";
        if (auto given_array = name_node[FastFHIR::Fields::HUMANNAME::GIVEN]) {
            for (auto& given : given_array.entries())
                std::cout << "given=" << given.as<std::string_view>() << "\n";
        }
    }
}
```

---

## 3. Memory Arena Creation and Reuse Patterns

### Anonymous RAM Arena
```cpp
auto mem = FastFHIR::Memory::create(/*Optionally provide arena upper bounds (something like 4 GB)*/);
```

### File-Backed Arena (Creation)
```cpp
// Map the arena straight to a file — every write goes directly to disk
auto mem = FastFHIR::Memory::createFromFile("patient.ffhr", 64 * 1024 * 1024);
```

### Reusing an Existing File-Backed Arena
```cpp
// Re-open the sealed patient.ffhr from previous tests and apply a second
// surgical mutation without going through JSON — set deceased to false.
auto mem = FastFHIR::Memory::createFromFile("patient.ffhr", 64 * 1024 * 1024);
FastFHIR::Builder builder(mem, FHIR_VERSION_R5);

auto patient = builder.root_handle();
REQUIRE(patient, "root_handle is null — archive must be sealed before second edit");

patient[FastFHIR::Fields::PATIENT::DECEASED] = false;

builder.finalize(FF_CHECKSUM_SHA256, sha256);
```

### Key Pattern: Memory Arena is Powerful
```cpp
// A Memory arena is much more powerful than a simple filestream or memory buffer.
// It can create a Memory::Streamhead into which a network socket can directly 
// stream network data and the Memory can safely (and lockelessly) write FastFHIR 
// data into the same archive from multiple concurrent threads.
// If unsure, always use a FastFHIR::Memory arena.
```

---

## 4. Wire Transfer / StreamHead Patterns

### Socket Receive into Arena via StreamHead
**From test_readme.cpp — Example 4:**
```cpp
// Receive exactly expected bytes from a socket straight into a FastFHIR arena.
// This exercises Memory::StreamHead, which is the intended NIC->arena ingress
// path for framed protocols.
static void socket_recv_exact_to_memory(asio::ip::tcp::socket &in_sock, 
                                        FastFHIR::Memory &dst, 
                                        size_t expected)
{
    dst.reset(0); // Start the stream at absolute offset 0 for a full archive copy.

    auto head_opt = dst.try_acquire_stream();
    REQUIRE(head_opt.has_value(), "failed to acquire stream lock on destination Memory");
    auto &head = *head_opt;

    size_t received = 0;
    while (received < expected)
    {
        size_t want = std::min(head.available_space(), expected - received);
        REQUIRE(want > 0, "destination arena has no space left during socket receive");

        asio::error_code ec;
        const size_t n = in_sock.read_some(asio::buffer(head.write_ptr(), want), ec);
        if (ec) {
            throw std::runtime_error("asio read_some failed: " + ec.message());
        }
        REQUIRE(n > 0, "socket closed before full archive was received");

        head.commit(n);
        received += n;
    }
}
```

### Complete Socket Round-Trip Pattern
**From test_readme.cpp — Example 4:**
```cpp
// Anonymous arena — no file backing
auto mem = FastFHIR::Memory::create(64 * 1024 * 1024);
FastFHIR::Builder builder(mem, FHIR_VERSION_R5);
FastFHIR::Ingest::Ingestor ingestor;

FastFHIR::Reflective::ObjectHandle patient_handle;
size_t count = 0;
auto result = ingestor.ingest(
    {builder, FastFHIR::Ingest::SourceType::FHIR_JSON, json_str},
    patient_handle, count);
REQUIRE(result.code == FF_SUCCESS, "ingest failed: " + result.message);

// Enrich in place — amend the birthDate (null after ingest; appends an FF_STRING block)
patient_handle[FastFHIR::Fields::PATIENT::BIRTH_DATE] = std::string_view("1990-03-21");

// Seal the stream and expose a zero-copy egress view.
// This is exactly what a network layer would write to a socket.
builder.set_root(patient_handle);
auto view = builder.finalize(FF_CHECKSUM_SHA256, sha256);
REQUIRE(!view.empty(), "finalize returned empty view");

// Real socket test:
// 1) send sealed archive bytes over a stream socket
// 2) receive into a different FastFHIR arena via StreamHead
// 3) parse on the receiver side and verify semantic integrity
LoopbackSocketPair sp;
socket_send_all(sp.client, view.data(), view.size());
asio::error_code shutdown_ec;
sp.client.shutdown(asio::ip::tcp::socket::shutdown_send, shutdown_ec);

// Slightly larger than payload to mimic a pre-allocated receive arena.
auto rx_mem = FastFHIR::Memory::create(view.size() + (4 * 1024));
socket_recv_exact_to_memory(sp.server, rx_mem, view.size());

auto socket_root = FastFHIR::Parser(rx_mem).root();
REQUIRE(socket_root, "socket-ingested root is null");

PatientData socket_data = socket_root;
REQUIRE(socket_data.id == "patient-1", "socket path changed patient id");
REQUIRE(socket_data.active == 1, "socket path changed active flag");
REQUIRE(socket_data.birthdate == "1990-03-21", "socket path changed birthDate");
```

---

## 5. JSON Export / Output Patterns

### C++ — Zero-Copy JSON Serialization
**From test_readme.cpp — Example 2:**
```cpp
// Also verify via the zero-copy JSON serializer
std::ostringstream oss;
root.print_json(oss);
const auto json = oss.str();
REQUIRE(json.find("patient-1") != std::string::npos, "id not in JSON output");
REQUIRE(json.find("Landvater") != std::string::npos, "family name not in JSON output");
std::cout << "  json[0..80] : " << json.substr(0, 80) << "...\n";
```

### C++ — Full FF_Export Implementation Pattern
**From tools/exporter/FF_Export.cpp:**
```cpp
#include "FF_Parser.hpp"
#include <iostream>
#include <fstream>

using namespace FastFHIR;

int main(int argc, char** argv) {
    std::string input_file;
    std::string output_file;

    // Parse arguments (omitted for brevity)
    
    try {
        const BYTE* parse_buffer = nullptr;
        size_t parse_size = 0;
        
        std::unique_ptr<MemoryMappedFile> mapped_file;
        std::vector<BYTE> stdin_buffer;

        // Resolve Input Strategy
        if (!input_file.empty()) {
            mapped_file = std::make_unique<MemoryMappedFile>(input_file);
            parse_buffer = mapped_file->data();
            parse_size = mapped_file->size();
        } else {
            // No input file provided; read from standard input
            std::ios_base::sync_with_stdio(false); // Speed up stdin
            std::cin.tie(NULL);
            stdin_buffer = read_stream_to_buffer(std::cin);
            
            if (stdin_buffer.empty()) {
                std::cerr << "Error: No input data received from stdin.\n";
                return 1;
            }
            parse_buffer = stdin_buffer.data();
            parse_size = stdin_buffer.size();
        }

        // Mount the Parser
        FastFHIR::Parser parser(parse_buffer, parse_size);

        // Resolve Output Strategy
        if (!output_file.empty()) {
            std::ofstream out_stream(output_file, std::ios::binary);
            if (!out_stream) throw std::runtime_error("Failed to open output file for writing.");
            parser.print_json(out_stream);
            out_stream << "\n";
        } else {
            // Write to stdout
            parser.print_json(std::cout);
            std::cout << "\n";
        }

    } catch (const std::exception& e) {
        std::cerr << "FastFHIR Export Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
```

### Python — JSON Export via to_json()
**From python/README.md — Example 2:**
```python
import fastfhir as ff
import json
from fastfhir.fields import Patient, HumanName

mem = ff.Memory.create_from_file("patient.ffhr", capacity=64 * 1024 * 1024)

with ff.Stream(mem, ff.FhirVersion.R5) as stream:
    patient_node = stream.root
    if not patient_node:
        raise RuntimeError("stream.root is null; archive root must be set before read.")

    # Materialize as plain Python dict/list
    patient_dict = json.loads(patient_node.to_json())
    for human_name in patient_dict.get("name", []):
        print(human_name["family"], human_name.get("given", []))

mem.close()
```

### Python — Inline JSON Output
```python
# Inspect while still in the stream
print(patient_node[Patient.ID].value())       # "patient-1"
print(patient_node[Patient.GENDER].value())   # "male"
print(patient_node[Patient.ACTIVE].value())   # True

# to_json() returns JSON text of this field
json_str = patient_node.to_json()
```

---

## 6. Complete Ingestion Workflow (FHIR JSON → .ffhr)

### C++ — Full Ingest, Enrich, and Seal
**From test_readme.cpp — Example 1:**
```cpp
std::string json_str = /* read patient.json */;

// Map the arena straight to a file — every write goes directly to disk
auto mem = FastFHIR::Memory::createFromFile("patient.ffhr", 64 * 1024 * 1024);
FastFHIR::Builder builder(mem, FHIR_VERSION_R5);
FastFHIR::Ingest::Ingestor ingestor;

FastFHIR::Reflective::ObjectHandle patient_handle;
size_t count = 0;
auto result = ingestor.ingest(
    {builder, FastFHIR::Ingest::SourceType::FHIR_JSON, json_str},
    patient_handle, count);

REQUIRE(result.code == FF_SUCCESS, "ingest failed: " + result.message);
REQUIRE(count > 0, "ingest parsed 0 resources");
REQUIRE(patient_handle, "patient handle is null after ingest");

// Inspect via zero-copy snapshot — no heap allocation for the read path
PatientData data = patient_handle.as_node();
std::cout << "  id     : " << data.id << "\n";
std::cout << "  gender : " << FF_AdministrativeGenderToString(data.gender) << "\n";
std::cout << "  active : " << (data.active == 1 ? "true" : "false") << "\n";

// Typed resource keys carry the exact write metadata (kind/offset/recovery).
patient_handle[FastFHIR::Fields::PATIENT::BIRTH_DATE] = std::string_view("1990-03-21");
patient_handle[FastFHIR::Fields::PATIENT::ACTIVE] = true;

// Seal with a SHA-256 footer — writes header + hash into the mapped pages
builder.set_root(patient_handle);
auto view = builder.finalize(FF_CHECKSUM_SHA256, [](const unsigned char* data, size_t len) {
    std::vector<uint8_t> hash(SHA256_DIGEST_LENGTH);
    SHA256(data, len, hash.data());
    return hash;
});
// patient.ffhr is now a valid, portable FastFHIR archive
```

### C++ — Using Anonymous Arena (In-Process)
**From README.md — Getting Started Step 3:**
```cpp
#include <FastFHIR.hpp>
#include <FF_FieldKeys.hpp>
#include <FF_Ingestor.hpp>

// Use an anonymous arena for this example — swap in createFromFile() to persist to disk.
auto mem = FastFHIR::Memory::create(/*Optionally provide arena upper bounds (something like 4 GB)*/);

FastFHIR::Builder          builder(mem, FHIR_VERSION_R5);
FastFHIR::Ingest::Ingestor ingestor;

// Any valid FHIR R4/R5 Patient JSON string.
std::string json = R"({
    "resourceType": "Patient",
    "id": "patient-1",
    "gender": "male",
    "name": [{"family": "Smith", "given": ["John"]}]
})";

// Ingest: converts JSON → binary in a single pass, writes into the arena.
FastFHIR::Reflective::ObjectHandle patient_handle;
size_t parsed_count = 0;
ingestor.ingest({builder, FastFHIR::Ingest::SourceType::FHIR_JSON, json},
                patient_handle, parsed_count);

// Enrich fields using typed resource keys.
patient_handle[FastFHIR::Fields::PATIENT::ACTIVE] = true;
patient_handle[FastFHIR::Fields::PATIENT::BIRTH_DATE] = "1990-03-21";

// Seal the stream (no checksum for brevity; see API Examples for SHA-256).
builder.set_root(patient_handle);
auto view = builder.finalize();      // returns a lifetime-safe Memory::View

// Read it back immediately — zero copies, same arena pages.
FastFHIR::Parser parser(view.data(), view.size());
auto root              = parser.root();
std::string_view id       = root[FastFHIR::Fields::PATIENT::ID];         // "patient-1"
bool             active   = root[FastFHIR::Fields::PATIENT::ACTIVE].as<bool>();  // true
std::string_view gender   = root[FastFHIR::Fields::PATIENT::GENDER];     // "male"
std::string_view birthdate = root[FastFHIR::Fields::PATIENT::BIRTH_DATE]; // "1990-03-21"
std::cout << "id=" << id << "  active=" << active
          << "  gender=" << gender << "  birthdate=" << birthdate << "\n";
```

### Python — Ingest and Save as .ffhr
**From python/README.md — Example 1:**
```python
import fastfhir as ff
from fastfhir.fields import Patient, HumanName

ingestor = ff.Ingestor()

with open("patient.json") as f:
    json_string = f.read()

# Map the arena straight to a file — every write goes directly to disk
mem = ff.Memory.create_from_file("patient.ffhr", capacity=64 * 1024 * 1024)

with ff.Stream(mem, ff.FhirVersion.R5) as stream:
    patient_node, _ = ingestor.ingest(stream, ff.SourceType.FHIR_JSON, json_string)

    # Inspect while still in the stream
    print(patient_node[Patient.ID].value())       # "patient-1"
    print(patient_node[Patient.GENDER].value())   # "male"
    print(patient_node[Patient.ACTIVE].value())   # True

    for name_entry in patient_node[Patient.NAME]:
        name   = name_entry.value()                                   # StreamNode
        family = name[HumanName.FAMILY].value()                    # str
        given  = [g.value() for g in name[HumanName.GIVEN]]       # list[str]
        print(given, family)   # ['Ryan', 'Eric'] Landvater

    # Seal the file — writes the header + SHA-256 footer into the mapped pages
    stream.finalize(algo=ff.Checksum.SHA256)

mem.close()   # patient.ffhr is now a valid, portable FastFHIR archive
```

---

## 7. Checksum Algorithms

### Available Options
```cpp
FF_CHECKSUM_NONE       // No footer
FF_CHECKSUM_CRC32      // CRC-32
FF_CHECKSUM_MD5        // MD5
FF_CHECKSUM_SHA256     // SHA-256
```

### Using in Finalize
```cpp
// No checksum
builder.finalize();

// With CRC32
builder.finalize(FF_CHECKSUM_CRC32);

// With SHA-256 and custom hasher
builder.finalize(FF_CHECKSUM_SHA256, [](const unsigned char* data, size_t len) {
    std::vector<uint8_t> hash(EVP_MAX_MD_SIZE);
    unsigned int out_len = 0;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, hash.data(), &out_len);
    EVP_MD_CTX_free(ctx);
    hash.resize(out_len);
    return hash;
});
```

---

## 8. Compact Archives (Post-Finalize Archival Transform)

### C++ — Create Compact Archive
**From test_readme.cpp — Example 7:**
```cpp
auto src_mem = FastFHIR::Memory::createFromFile("patient.ffhr", 64 * 1024 * 1024);
FastFHIR::Parser src_parser(src_mem);

auto compact_mem = FastFHIR::Memory::createFromFile("patient.compact.ffhr", 64 * 1024 * 1024);
auto compact_view = FastFHIR::Compactor::archive(src_parser, compact_mem);
REQUIRE(!compact_view.empty(), "compact archive view is empty");

// Read the compact archive — identical typed-key API
FastFHIR::Parser compact_parser(compact_mem);
auto root = compact_parser.root();

std::string_view id     = root[FastFHIR::Fields::PATIENT::ID];          // "patient-1"
std::string_view gender = root[FastFHIR::Fields::PATIENT::GENDER];      // "male"
bool             active = root[FastFHIR::Fields::PATIENT::ACTIVE].as<bool>(); // true

for (auto& name_node : root[FastFHIR::Fields::PATIENT::NAME].entries()) {
    std::string_view family = name_node[FastFHIR::Fields::HUMANNAME::FAMILY];
    for (auto& g : name_node[FastFHIR::Fields::HUMANNAME::GIVEN].entries())
        std::cout << g.as<std::string_view>() << " ";
    std::cout << family << "\n";
}
```

### Python — Create Compact Archive
**From python/README.md — Example 6:**
```python
import fastfhir as ff

# Source: any previously finalized .ffhr file.
src_mem     = ff.Memory.create_from_file("patient.ffhr",         capacity=64 * 1024 * 1024)
dest_mem    = ff.Memory.create_from_file("patient.compact.ffhr", capacity=64 * 1024 * 1024)

with ff.Stream(src_mem, ff.FhirVersion.R5) as stream:
    # compact() resets dest_mem, writes the dense archive, and seals it.
    # The source stream and its backing arena are untouched.
    compact_view = stream.compact(dest_mem, algo=ff.Checksum.SHA256)
    print(f"original : {src_mem.size:,} bytes")
    print(f"compact  : {compact_view.size:,} bytes")

src_mem.close()
dest_mem.close()   # patient.compact.ffhr is a sealed compact FastFHIR archive
```

---

## 9. Surgical In-Place Enrichment (Append-Only, Zero-Copy)

### Re-open and Enrich
**From test_readme.cpp — Example 3:**
```cpp
// Mount the existing archive — stays mapped to the same file
auto mem = FastFHIR::Memory::createFromFile("patient.ffhr", 64 * 1024 * 1024);
FastFHIR::Builder builder(mem, FHIR_VERSION_R5);

auto patient = builder.root_handle();
REQUIRE(patient, "root_handle is null — archive must be sealed before enrichment");

// Amend a string field — appends a new FF_STRING block and patches the pointer slot.
// The original record bytes are never touched; only a new tail is written.
patient[FastFHIR::Fields::PATIENT::BIRTH_DATE] = std::string_view("1990-03-21");

// Re-seal — old data untouched, new tail written
builder.finalize(FF_CHECKSUM_SHA256, sha256);

// Re-open and verify the enriched record
auto mem2 = FastFHIR::Memory::createFromFile("patient.ffhr", 64 * 1024 * 1024);
auto root = FastFHIR::Parser(mem2).root();
REQUIRE(root, "root is null after re-seal");

PatientData data = root;
REQUIRE(!data.birthdate.empty(), "birthDate should be non-empty after enrichment");
std::cout << "  birthDate: " << data.birthdate << "\n";
REQUIRE(data.birthdate == "1990-03-21", "unexpected birthDate value");
```

### Bundle Entry Surgical Edit
**From test_readme.cpp — Example 5:**
```cpp
// ── Step A: ingest the bundle ──
auto mem = FastFHIR::Memory::createFromFile("bundle.ffhr", 64 * 1024 * 1024);
FastFHIR::Builder builder(mem, FHIR_VERSION_R5);
FastFHIR::Ingest::Ingestor ingestor;

FastFHIR::Reflective::ObjectHandle bundle_handle;
size_t count = 0;
auto result = ingestor.ingest(
    {builder, FastFHIR::Ingest::SourceType::FHIR_JSON, BUNDLE_JSON},
    bundle_handle, count);

// Seal the initial bundle
builder.set_root(bundle_handle);
builder.finalize(FF_CHECKSUM_SHA256, sha256);

// ── Step B: re-open and find patient-1 ──
auto mem2 = FastFHIR::Memory::createFromFile("bundle.ffhr", 64 * 1024 * 1024);
FastFHIR::Builder builder2(mem2, FHIR_VERSION_R5);
auto root2 = FastFHIR::Parser(mem2).root();

BundleData bundle_data = root2;
REQUIRE(!bundle_data.entry.empty(), "bundle.entry is empty");

// Walk entries; the OS faults in only the pages we read
FastFHIR::Reflective::ObjectHandle target_patient;
for (auto &entry : bundle_data.entry)
{
    if (entry.resource.recovery != FF_PATIENT::recovery)
        continue;

    auto patient_data = FF_PATIENT::deserialize(
        mem2.base(),
        entry.resource.offset,
        mem2.capacity(),
        FHIR_VERSION_R5);

    if (patient_data.id == "patient-1")
    {
        target_patient = FastFHIR::Reflective::ObjectHandle(
            &builder2,
            entry.resource.offset,
            entry.resource.recovery);
        break;
    }
}

// ── Step C: surgically enrich patient-1 only ──
auto res = ingestor.insert_at_field(
    target_patient, FastFHIR::Fields::PATIENT::TELECOM,
    R"({"system":"phone","value":"555-0199","use":"mobile"})");
REQUIRE(res.code == FF_SUCCESS, "surgical enrich failed: " + res.message);

// ── Step D: reseal — only the header + new tail pages are rewritten ──
auto bundle_root = builder2.root_handle();
REQUIRE(bundle_root, "bundle root handle is null after enrichment");
builder2.finalize(FF_CHECKSUM_SHA256, sha256);
```

---

## 10. Typed Field Keys Reference

### Scalar Fields
```cpp
FastFHIR::Fields::PATIENT::ID          // "id"          (string)
FastFHIR::Fields::PATIENT::ACTIVE      // "active"      (bool)
FastFHIR::Fields::PATIENT::GENDER      // "gender"      (code)
FastFHIR::Fields::PATIENT::BIRTH_DATE  // "birthDate"   (string)
FastFHIR::Fields::PATIENT::DECEASED    // "deceased"    (bool)
```

### Array Fields
```cpp
FastFHIR::Fields::PATIENT::NAME        // "name"        (array of HumanName)
FastFHIR::Fields::PATIENT::TELECOM     // "telecom"     (array of ContactPoint)
FastFHIR::Fields::PATIENT::ADDRESS     // "address"     (array of Address)
```

### Nested Object Fields
```cpp
FastFHIR::Fields::HUMANNAME::FAMILY    // "family"      (string)
FastFHIR::Fields::HUMANNAME::GIVEN     // "given"       (array of string)
FastFHIR::Fields::HUMANNAME::USE       // "use"         (code)
```

### Bundle-Specific
```cpp
FastFHIR::Fields::BUNDLE::ENTRY        // "entry"       (bundle entry array)
FastFHIR::Fields::BUNDLE_ENTRY::RESOURCE // "resource"  (polymorphic resource)
```

### Observation-Specific
```cpp
FastFHIR::Fields::OBSERVATION::STATUS  // "status"      (code)
FastFHIR::Fields::OBSERVATION::CODE    // "code"        (CodeableConcept)
FastFHIR::Fields::OBSERVATION::VALUE   // "value"       (polymorphic — Quantity, string, etc.)
```

### CodeableConcept
```cpp
FastFHIR::Fields::CODING::SYSTEM       // "system"      (string)
FastFHIR::Fields::CODING::CODE         // "code"        (string)
FastFHIR::Fields::CODING::DISPLAY      // "display"     (string)
```

---

## 11. FHIR Version Constants

```cpp
FHIR_VERSION_R4   // HL7 FHIR R4
FHIR_VERSION_R5   // HL7 FHIR R5 (default)
```

---

## 12. Error Handling Pattern

```cpp
FastFHIR::Reflective::ObjectHandle patient_handle;
size_t count = 0;
auto result = ingestor.ingest(
    {builder, FastFHIR::Ingest::SourceType::FHIR_JSON, json_str},
    patient_handle, count);

REQUIRE(result.code == FF_SUCCESS, "ingest failed: " + result.message);
REQUIRE(count > 0, "ingest parsed 0 resources");
REQUIRE(patient_handle, "patient handle is null after ingest");
```

---

## 13. Python-Specific Patterns

### Field Access (Python)
```python
import fastfhir as ff
from fastfhir.fields import Patient, HumanName

patient_node[Patient.ID]            # Returns MutableEntry
patient_node[Patient.ACTIVE].value() # Returns Python bool or None
patient_node[Patient.NAME].value()   # Returns StreamNode (block/array) or None
```

### Iteration (Python)
```python
# Iterate array entries
for name_entry in patient_node[Patient.NAME]:
    name = name_entry.value()  # Get StreamNode
    family = name[HumanName.FAMILY].value()  # Get str
    given = [g.value() for g in name[HumanName.GIVEN]]  # List of str

# Get present fields as dict/list
items = patient_node.items(recursive=True)  # list[(str, native)]
```

### Resource Type Check (Python)
```python
RT = ff._core.ResourceType

resource_node = entry[BundleEntry.RESOURCE].value()
if resource_node.recovery_tag == RT.Patient:
    print("This is a Patient resource")
```

---

## Summary: Key Canonical Patterns

1. **Loading FFHR:** Always use `Memory::createFromFile()` for persistent files, `Memory::create()` for in-process
2. **Parsing:** Construct `Parser` with Memory or raw bytes; use `.root()` and typed field keys
3. **Ingestion:** Use `Builder` + `Ingest::Ingestor` for JSON → FFHR conversion
4. **Enrichment:** Append-only with typed field keys; old data never touched, only new tail
5. **Export:** Use `Parser::print_json()` for C++, `node.to_json()` for Python
6. **Checksums:** `finalize(FF_CHECKSUM_SHA256, callback)` with custom hasher for security
7. **Socket Transport:** Use `Memory::StreamHead` via `try_acquire_stream()` for zero-copy ingest
8. **Compaction:** Create compact archives post-finalize for reduced wire size (read-only)

