#pragma once                                        // 防止头文件被重复包含
#include <string>                                   // 引入 std::string 字符串类型
#include <vector>                                   // 引入 std::vector 动态数组
#include <utility>                                  // 引入 std::pair 等通用工具组件
#include <iostream>                                 // 引入标准输入输出流
#include <sstream>                                  // 引入 std::stringstream 字符串流
#include <memory>                                   // 引入智能指针（std::shared_ptr 等）
#include <functional>                               // 引入 std::function 可调用对象包装器
#include <unordered_map>                            // 引入 std::unordered_map 哈希表


#include"AIStrategy.h"                              // 引入 AI 策略基类及各具体策略类定义

class StrategyFactory {                             // 策略工厂类：负责按名称注册与创建 AI 策略对象

public:
    using Creator = std::function<std::shared_ptr<AIStrategy>()>;   // 定义创建器类型：无参、返回 AIStrategy 智能指针的可调用对象

    static StrategyFactory& instance();             // 单例访问接口：返回全局唯一的工厂实例

    void registerStrategy(const std::string& name, Creator creator);   // 以名称为键注册一个策略创建器

    std::shared_ptr<AIStrategy> create(const std::string& name);       // 根据名称创建对应的策略实例

private:
    StrategyFactory() = default;                    // 默认构造函数设为私有，禁止外部直接构造（保证单例）
    std::unordered_map<std::string, Creator> creators;   // 名称到创建器的映射表：name -> 创建函数
};





template<typename T>                                // 模板声明：T 为具体的 AIStrategy 子类类型
struct StrategyRegister {                           // 策略自动注册器：构造时即把类型 T 注册进工厂
    StrategyRegister(const std::string& name) {     // 构造函数：接收注册名称
        StrategyFactory::instance().registerStrategy(name, [] {   // 向单例工厂注册名为 name 的创建器（lambda）
            std::shared_ptr<AIStrategy> instance = std::make_shared<T>();   // 创建器被调用时：new 一个 T 实例并用智能指针持有
            return instance;                        // 返回创建好的策略实例
            });                                     // lambda 定义与 registerStrategy 调用结束
    }                                               // 构造函数结束
};                                                  // StrategyRegister 结构体定义结束

