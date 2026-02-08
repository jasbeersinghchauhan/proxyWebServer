#pragma once

#include <string>
#include <memory>
#include <mutex>
#include <fstream>
#include <format>
#include <string_view>


class ProxyLogger
{
public:
    static ProxyLogger &getInstance();

    template <typename... Args>
    void internal_log(std::string_view fmt, Args &&...args)
    {
        log_formatted(fmt, std::make_format_args(args...));
    }

    ProxyLogger(const ProxyLogger &) = delete;
    ProxyLogger &operator=(const ProxyLogger &) = delete;

    void log_formatted(std::string_view fmt, std::format_args args);

private:
    ProxyLogger();
    ~ProxyLogger();

    std::unique_ptr<std::ofstream> m_file;
    std::mutex m_mutex;
};

template <typename... Args>
void log(std::string_view fmt, Args &&...args)
{
    ProxyLogger::getInstance().internal_log(fmt, std::forward<Args>(args)...);
}