#include <aeron_cluster/sbe_messages.hpp>
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <iomanip>
#include <sstream>
#include <cstring>

using namespace aeron_cluster;

/**
 * @brief Message Inspector Tool
 * 
 * This tool helps debug SBE messages by:
 * - Parsing binary message files
 * - Decoding hex strings
 * - Validating message format
 * - Displaying detailed message content
 */

void printUsage(const char* programName) {
    std::cout << "Aeron Cluster Message Inspector" << std::endl;
    std::cout << "===============================" << std::endl;
    std::cout << std::endl;
    std::cout << "Usage: " << programName << " [options]" << std::endl;
    std::cout << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  --file FILE        Inspect binary message file" << std::endl;
    std::cout << "  --hex STRING       Inspect hex-encoded message" << std::endl;
    std::cout << "  --test-encoding    Test SBE encoding/decoding" << std::endl;
    std::cout << "  --generate TYPE    Generate sample message (session-connect|topic|ack)" << std::endl;
    std::cout << "  --verbose          Enable verbose output" << std::endl;
    std::cout << "  --help             Show this help message" << std::endl;
    std::cout << std::endl;
    std::cout << "Examples:" << std::endl;
    std::cout << "  " << programName << " --file message.bin" << std::endl;
    std::cout << "  " << programName << " --hex \"0A1B2C3D4E5F\"" << std::endl;
    std::cout << "  " << programName << " --test-encoding" << std::endl;
    std::cout << "  " << programName << " --generate topic --verbose" << std::endl;
    std::cout << std::endl;
}

std::vector<uint8_t> hexStringToBytes(const std::string& hex) {
    std::vector<uint8_t> bytes;
    
    // Remove spaces and make uppercase
    std::string cleanHex;
    for (char c : hex) {
        if (std::isxdigit(c)) {
            cleanHex += std::toupper(c);
        }
    }
    
    // Must have even number of hex characters
    if (cleanHex.length() % 2 != 0) {
        throw std::runtime_error("Invalid hex string: must have even number of characters");
    }
    
    for (size_t i = 0; i < cleanHex.length(); i += 2) {
        std::string byteString = cleanHex.substr(i, 2);
        uint8_t byte = static_cast<uint8_t>(std::stoi(byteString, nullptr, 16));
        bytes.push_back(byte);
    }
    
    return bytes;
}

std::vector<uint8_t> readBinaryFile(const std::string& filename) {
    std::ifstream file(filename, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Cannot open file: " + filename);
    }
    
    // Get file size
    file.seekg(0, std::ios::end);
    size_t fileSize = file.tellg();
    file.seekg(0, std::ios::beg);
    
    // Read entire file
    std::vector<uint8_t> data(fileSize);
    file.read(reinterpret_cast<char*>(data.data()), fileSize);
    
    if (!file) {
        throw std::runtime_error("Error reading file: " + filename);
    }
    
    return data;
}

void inspectMessage(const std::vector<uint8_t>& data, bool verbose) {
    if (data.empty()) {
        std::cout << "❌ Empty message data" << std::endl;
        return;
    }
    
    std::cout << "📋 Message Inspector Results" << std::endl;
    std::cout << "============================" << std::endl;
    std::cout << "Message size: " << data.size() << " bytes" << std::endl;
    std::cout << std::endl;
    
    // Print hex dump if verbose or small message
    if (verbose || data.size() <= 64) {
        std::cout << "📋 Hex Dump:" << std::endl;
        SBEUtils::print_hex_dump(data.data(), data.size(), "  ");
        std::cout << std::endl;
    }
    
    // Check if it looks like a valid SBE message
    if (!SBEUtils::is_valid_sbe_message(data.data(), data.size())) {
        std::cout << "⚠️  Warning: Data doesn't look like a valid SBE message" << std::endl;
        
        // Try to extract readable strings
        auto strings = SBEUtils::extract_readable_strings(data.data(), data.size(), 3);
        if (!strings.empty()) {
            std::cout << "📝 Readable strings found:" << std::endl;
            for (const auto& str : strings) {
                std::cout << "  \"" << str << "\"" << std::endl;
            }
        }
        std::cout << std::endl;
        return;
    }
    
    // Parse the message
    ParseResult result = MessageParser::parse_message(data.data(), data.size());
    
    std::cout << "📊 Parse Results:" << std::endl;
    std::cout << "  Success: " << (result.success ? "✅ Yes" : "❌ No") << std::endl;
    
    if (!result.success) {
        std::cout << "  Error: " << result.error_message << std::endl;
        std::cout << std::endl;
        return;
    }
    
    std::cout << "  Message Type: " << result.get_description() << std::endl;
    std::cout << "  Template ID: " << result.template_id << std::endl;
    std::cout << "  Schema ID: " << result.schema_id << std::endl;
    std::cout << "  Version: " << result.version << std::endl;
    std::cout << "  Block Length: " << result.block_length << std::endl;
    
    if (result.timestamp > 0) {
        std::cout << "  Timestamp: " << SBEUtils::format_timestamp(result.timestamp) << std::endl;
    }
    
    std::cout << std::endl;
    
    // Message-specific details
    if (result.is_session_event()) {
        std::cout << "📨 Session Event Details:" << std::endl;
        std::cout << "  Correlation ID: " << result.correlation_id << std::endl;
        std::cout << "  Session ID: " << result.session_id << std::endl;
        std::cout << "  Leader Member: " << result.leader_member_id << std::endl;
        std::cout << "  Event Code: " << result.event_code 
                 << " (" << SBEUtils::get_session_event_code_string(result.event_code) << ")" << std::endl;
        
        if (!result.payload.empty()) {
            std::cout << "  Detail: " << result.payload << std::endl;
        }
    } else if (result.is_topic_message()) {
        std::cout << "📨 Topic Message Details:" << std::endl;
        std::cout << "  Message Type: " << result.message_type << std::endl;
        std::cout << "  Message ID: " << result.message_id << std::endl;
        
        if (!result.payload.empty()) {
            std::cout << "  Payload: " << result.payload.substr(0, 200);
            if (result.payload.length() > 200) {
                std::cout << "... (" << (result.payload.length() - 200) << " more chars)";
            }
            std::cout << std::endl;
        }
        
        if (!result.headers.empty()) {
            std::cout << "  Headers: " << result.headers << std::endl;
        }
    } else if (result.is_acknowledgment()) {
        std::cout << "📨 Acknowledgment Details:" << std::endl;
        std::cout << "  Message ID: " << result.message_id << std::endl;
        std::cout << "  Status: " << result.payload << std::endl;
        
        if (!result.headers.empty()) {
            std::cout << "  Error: " << result.headers << std::endl;
        }
    }
    
    std::cout << std::endl;
}

void testEncoding(bool verbose) {
    std::cout << "🧪 Testing SBE Encoding/Decoding" << std::endl;
    std::cout << "=================================" << std::endl;
    
    // Test 1: SessionConnectRequest
    std::cout << "Test 1: SessionConnectRequest" << std::endl;
    try {
        int64_t correlation_id = 123456789;
        int32_t responseStreamId = 102;
        std::string responseChannel = "aeron:udp?endpoint=localhost:44445";
        int32_t protocolVersion = 1;
        
        auto encoded = SBEEncoder::encode_session_connect_request(
            correlation_id, responseStreamId, responseChannel, protocolVersion);
        
        std::cout << "  ✅ Encoding successful (" << encoded.size() << " bytes)" << std::endl;
        
        if (verbose) {
            std::cout << "  Hex dump:" << std::endl;
            SBEUtils::print_hex_dump(encoded.data(), encoded.size(), "    ");
        }
        
        // Try to parse it back
        ParseResult result = MessageParser::parse_message(encoded.data(), encoded.size());
        if (result.success && result.is_session_event()) {
            std::cout << "  ✅ Round-trip test failed - parsed as wrong type" << std::endl;
        } else {
            std::cout << "  ℹ️  Note: SessionConnectRequest can't be parsed as response message" << std::endl;
        }
        
    } catch (const std::exception& e) {
        std::cout << "  ❌ Test failed: " << e.what() << std::endl;
    }
    
    std::cout << std::endl;
    
    // Test 2: TopicMessage
    std::cout << "Test 2: TopicMessage" << std::endl;
    try {
        std::string topic = "orders";
        std::string messageType = "CREATE_ORDER";
        std::string uuid = "msg_12345_67890";
        std::string payload = R"({"id":"order_123","side":"BUY","quantity":1.0})";
        std::string headers = R"({"messageId":"msg_12345_67890"})";
        
        auto encoded = SBEEncoder::encode_topic_message(
            topic, messageType, uuid, payload, headers);
        
        std::cout << "  ✅ Encoding successful (" << encoded.size() << " bytes)" << std::endl;
        
        if (verbose) {
            std::cout << "  Hex dump:" << std::endl;
            SBEUtils::print_hex_dump(encoded.data(), encoded.size(), "    ");
        }
        
        // Try to parse it back
        ParseResult result = MessageParser::parse_message(encoded.data(), encoded.size());
        if (result.success && result.is_topic_message()) {
            std::cout << "  ✅ Round-trip test successful" << std::endl;
            std::cout << "    Parsed message type: " << result.message_type << std::endl;
            std::cout << "    Parsed message ID: " << result.message_id << std::endl;
        } else {
            std::cout << "  ❌ Round-trip test failed: " << result.error_message << std::endl;
        }
        
    } catch (const std::exception& e) {
        std::cout << "  ❌ Test failed: " << e.what() << std::endl;
    }
    
    std::cout << std::endl;
}

void generateSampleMessage(const std::string& type, bool verbose) {
    std::cout << "🏭 Generating Sample " << type << " Message" << std::endl;
    std::cout << "==============================" << std::endl;
    
    try {
        std::vector<uint8_t> encoded;
        
        if (type == "session-connect") {
            encoded = SBEEncoder::encode_session_connect_request(
                987654321, 102, "aeron:udp?endpoint=localhost:0", 1);
        } else if (type == "topic") {
            encoded = SBEEncoder::encode_topic_message(
                "orders", "CREATE_ORDER", "sample_msg_123",
                R"({"id":"sample_order","side":"BUY","quantity":1.5,"price":3500.0})",
                R"({"messageId":"sample_msg_123","timestamp":1234567890})");
        } else {
            std::cout << "❌ Unknown message type: " << type << std::endl;
            std::cout << "Available types: session-connect, topic" << std::endl;
            return;
        }
        
        std::cout << "✅ Generated " << encoded.size() << " byte message" << std::endl;
        std::cout << std::endl;
        
        // Save to file
        std::string filename = "sample_" + type + "_message.bin";
        std::ofstream file(filename, std::ios::binary);
        file.write(reinterpret_cast<const char*>(encoded.data()), encoded.size());
        
        std::cout << "💾 Saved to file: " << filename << std::endl;
        std::cout << std::endl;
        
        // Print hex string for command line use
        std::cout << "📋 Hex string (for --hex option):" << std::endl;
        std::cout << "  ";
        for (size_t i = 0; i < encoded.size(); ++i) {
            std::cout << std::setfill('0') << std::setw(2) << std::hex 
                     << static_cast<unsigned>(encoded[i]);
        }
        std::cout << std::dec << std::endl;
        std::cout << std::endl;
        
        // Parse and display
        if (verbose) {
            std::cout << "📊 Message Analysis:" << std::endl;
            inspectMessage(encoded, false);
        }
        
    } catch (const std::exception& e) {
        std::cout << "❌ Generation failed: " << e.what() << std::endl;
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }
    
    std::string mode;
    std::string inputFile;
    std::string hexString;
    std::string generateType;
    bool verbose = false;
    
    // Parse command line arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        
        if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        } else if (arg == "--file" && i + 1 < argc) {
            mode = "file";
            inputFile = argv[++i];
        } else if (arg == "--hex" && i + 1 < argc) {
            mode = "hex";
            hexString = argv[++i];
        } else if (arg == "--test-encoding") {
            mode = "test";
        } else if (arg == "--generate" && i + 1 < argc) {
            mode = "generate";
            generateType = argv[++i];
        } else if (arg == "--verbose") {
            verbose = true;
        } else {
            std::cerr << "Unknown option: " << arg << std::endl;
            printUsage(argv[0]);
            return 1;
        }
    }
    
    try {
        if (mode == "file") {
            std::cout << "📁 Inspecting file: " << inputFile << std::endl;
            std::cout << std::endl;
            
            auto data = readBinaryFile(inputFile);
            inspectMessage(data, verbose);
            
        } else if (mode == "hex") {
            std::cout << "🔢 Inspecting hex string: " << hexString << std::endl;
            std::cout << std::endl;
            
            auto data = hexStringToBytes(hexString);
            inspectMessage(data, verbose);
            
        } else if (mode == "test") {
            testEncoding(verbose);
            
        } else if (mode == "generate") {
            generateSampleMessage(generateType, verbose);
            
        } else {
            std::cerr << "No valid mode specified" << std::endl;
            printUsage(argv[0]);
            return 1;
        }
        
    } catch (const std::exception& e) {
        std::cerr << "❌ Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}