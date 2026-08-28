#pragma once                                        // 防止头文件被重复包含
#include <chrono>                                   // 引入 std::chrono 时间库（时间戳相关）
#include <random>                                   // 引入随机数库（std::srand/std::rand）
#include <cstdlib>                                  // 引入 C 标准库（std::rand/std::srand 定义处）
#include <ctime>                                    // 引入 C 时间库（std::time 获取当前时间）
#include <string>                                   // 引入 std::string 字符串类型


class AISessionIdGenerator {                        // AI 会话 ID 生成器类：为每个对话会话生成唯一标识
public:
    AISessionIdGenerator() {                        // 构造函数：初始化随机数种子
                                                    // （空行：构造体开始）
        std::srand(static_cast<unsigned>(std::time(nullptr)));   // 用当前时间（秒级时间戳）作为随机数种子，保证每次运行生成的 ID 不同
    }                                               // 构造函数结束

    std::string generate();                         // 生成一个会话 ID 字符串（实现在 .cpp 文件中）
};                                                  // AISessionIdGenerator 类定义结束
