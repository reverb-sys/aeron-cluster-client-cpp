#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <mutex>
#include <chrono>
#include <vector>
#include <memory>
#include "aeron_cluster/protocol.hpp"

namespace aeron_cluster {

/**
 * @brief Represents a commit offset for a specific topic
 */
struct CommitOffset {
    std::string topic;
    std::string message_id;
    std::uint64_t timestamp_nanos;
    std::uint64_t sequence_number;
    
    CommitOffset() = default;
    CommitOffset(const std::string& t, const std::string& mid, std::uint64_t ts, std::uint64_t seq)
        : topic(t), message_id(mid), timestamp_nanos(ts), sequence_number(seq) {}
};

/**
 * @brief Manages message commits and resume functionality
 */
class CommitManager {
public:
    /**
     * @brief Construct commit manager
     */
    CommitManager();
    
    /**
     * @brief Destructor
     */
    ~CommitManager();

    // Non-copyable, non-movable
    CommitManager(const CommitManager&) = delete;
    CommitManager& operator=(const CommitManager&) = delete;
    CommitManager(CommitManager&&) = delete;
    CommitManager& operator=(CommitManager&&) = delete;

    /**
     * @brief Commit a message for a specific topic
     * @param topic Topic name
     * @param message_id Message ID to commit
     * @param timestamp_nanos Message timestamp
     * @param sequence_number Message sequence number
     */
    void commit_message(const std::string& topic, const std::string& message_id, 
                      std::uint64_t timestamp_nanos, std::uint64_t sequence_number);

    /**
     * @brief Get the last committed offset for a topic
     * @param topic Topic name
     * @return Last commit offset, or nullptr if no commits exist
     */
    std::shared_ptr<CommitOffset> get_last_commit(const std::string& topic) const;

    /**
     * @brief Get all committed topics and their offsets
     * @return Map of topic to commit offset
     */
    std::unordered_map<std::string, CommitOffset> get_all_commits() const;

    /**
     * @brief Clear all commits for a specific topic
     * @param topic Topic name
     */
    void clear_commits(const std::string& topic);

    /**
     * @brief Clear all commits
     */
    void clear_all_commits();

    /**
     * @brief Build commit message for a topic
     * @param topic Topic name
     * @param client_id Client identifier
     * @return SBE-encoded commit message
     */
    std::vector<std::uint8_t> build_commit_message(const std::string& topic, const std::string& client_id) const;

    /**
     * @brief Build commit offset message for a topic
     * @param topic Topic name
     * @param client_id Client identifier
     * @param offset Commit offset to send
     * @return SBE-encoded commit offset message
     */
    std::vector<std::uint8_t> build_commit_offset_message(const std::string& topic, const std::string& client_id, 
                                                         const CommitOffset& offset) const;

    /**
     * @brief Parse commit offset from JSON payload
     * @param payload JSON payload containing commit information
     * @return Parsed commit offset
     */
    static CommitOffset parse_commit_offset(const std::string& payload);

    /**
     * @brief Serialize commit offset to JSON
     * @param offset Commit offset to serialize
     * @return JSON string representation
     */
    static std::string serialize_commit_offset(const CommitOffset& offset);

private:
    mutable std::mutex commits_mutex_;
    std::unordered_map<std::string, CommitOffset> commits_;
};

} // namespace aeron_cluster
