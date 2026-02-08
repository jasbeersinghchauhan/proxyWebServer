#include "proxy_logger.hpp"
#include <chrono>

ProxyLogger& ProxyLogger::getInstance() {
    static ProxyLogger instance;
    return instance;
}

ProxyLogger::ProxyLogger() {
    m_file = std::make_unique<std::ofstream>("proxy.log", std::ios::app);
    
    if (m_file->is_open()) {
        *m_file << "[INFO]|SYSTEM|Logger initialized via unique_ptr.\n";
    }
}

ProxyLogger::~ProxyLogger() {
    if (m_file && m_file->is_open()) {
        m_file->close();
    }
}

void ProxyLogger::log_formatted(std::string_view fmt, std::format_args args) {
    if (!m_file || !m_file->is_open()) return;

    std::string message = std::vformat(fmt, args);
    
    const auto now = std::chrono::system_clock::now();
    const auto local_time = std::chrono::zoned_time{std::chrono::current_zone(), now};

    std::lock_guard<std::mutex> lock(m_mutex);

    *m_file << std::format("[{:%Y-%m-%d %H:%M:%S}] ", local_time) 
            << message;

    m_file->flush();
}