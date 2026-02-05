#ifndef PROXY_LOGGER_HPP
#define PROXY_LOGGER_HPP

#include <string>
#include <memory>
#include <mutex>
#include <fstream>
#include <format>
#include <string_view>

class ProxyLogger {
public:
    static ProxyLogger& getInstance();

    ProxyLogger(const ProxyLogger&) = delete;
    ProxyLogger& operator=(const ProxyLogger&) = delete;

    void log_formatted(std::string_view fmt, std::format_args args);

private:
    ProxyLogger(); 
    ~ProxyLogger();

    std::unique_ptr<std::ofstream> m_file;
    std::mutex m_mutex;
};

#define log(fmt, ...) \
    ProxyLogger::getInstance().log_formatted(fmt, std::make_format_args(__VA_ARGS__))
#endif