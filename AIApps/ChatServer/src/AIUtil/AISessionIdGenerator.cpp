#include"../include/AIUtil/AISessionIdGenerator.h"   // 引入会话 ID 生成器类的头文件声明




std::string AISessionIdGenerator::generate(){        // 实现 generate()：生成一个唯一的会话 ID 字符串
    auto now = std::chrono::system_clock::now().time_since_epoch().count();   // 获取当前时间戳（自 epoch 起的计时数，单位由时钟精度决定）
    long long randVal = std::rand() % 100000;        // 生成 0~99999 的随机数，增加 ID 的随机性 // 0~99999
    long long rawId = now ^ randVal;                 // 时间戳与随机数按位异或，混合得到原始 ID
    return std::to_string(rawId);                    // 将原始 ID 转为字符串并返回
}                                                    // generate() 实现结束
