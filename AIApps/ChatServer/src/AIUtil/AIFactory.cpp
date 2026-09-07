#include"../include/AIUtil/AIFactory.h"


StrategyFactory& StrategyFactory::instance() {
    static StrategyFactory factory;
    static const bool registered = [&] {
        factory.registerStrategy("1", [] { return std::make_shared<AliyunStrategy>(); });
        factory.registerStrategy("2", [] { return std::make_shared<DouBaoStrategy>(); });
        factory.registerStrategy("3", [] { return std::make_shared<AliyunRAGStrategy>(); });
        factory.registerStrategy("4", [] { return std::make_shared<AliyunMcpStrategy>(); });
        return true;
    }();
    (void)registered;
    return factory;
}

void StrategyFactory::registerStrategy(const std::string& name, Creator creator) {
    creators[name] = std::move(creator);
}

std::shared_ptr<AIStrategy> StrategyFactory::create(const std::string& name) {
    auto it = creators.find(name);
    if (it == creators.end()) {
        throw std::runtime_error("Unknown strategy: " + name);
    }
    return it->second();
}
