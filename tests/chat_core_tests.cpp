#include "AIUtil/AIHelper.h"
#include "http/ResponseStream.h"
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <thread>

using Clock = std::chrono::steady_clock;
static void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
class FakeStrategy : public AIStrategy {
public:
    std::string getApiUrl() const override { return "unused"; }
    std::string getApiKey() const override { return "unused"; }
    std::string getModel() const override { return "test"; }
    json buildRequest(const AIHelper::Messages&) const override { return {}; }
    std::string parseResponse(const json&) const override { return {}; }
    std::string chat(AIHelper::Messages& messages, const std::string& question,
                     std::function<json(const json&, bool)>) override {
        messages.emplace_back(question, 0);
        if (question == "fail") throw std::runtime_error("simulated failure");
        if (question == "slow") std::this_thread::sleep_for(std::chrono::milliseconds(350));
        return "answer";
    }
};
int main(int argc, char** argv) {
    try {
        if (argc == 3) {
            std::string mode = argv[2];
            auto start = Clock::now();
            int count = 0;
            auto result = ChatTransport::request(argv[1], "test-key", json{{"messages", json::array()}},
                mode != "plain", mode == "rag", [&](const std::string& delta) {
                    ++count;
                    std::cout << json{{"delta", delta}, {"elapsedMs", std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-start).count()}}.dump() << std::endl;
                }, [&] { return mode == "cancel" && Clock::now()-start > std::chrono::milliseconds(250); });
            std::cout << json{{"result", result}, {"count", count}}.dump() << std::endl;
            return 0;
        }
        std::vector<std::string> events;
        SseDecoder decoder([&](const std::string& value) { events.push_back(value); });
        std::string wire = ": ping\r\nevent: delta\r\ndata: 中文\r\ndata: second\r\n\r\ndata: [DONE]\n\n";
        for (char byte : wire) decoder.feed(&byte, 1);
        decoder.finish();
        check(events.size() == 2 && events[0] == "中文\nsecond" && events[1] == "[DONE]", "fragmented SSE/CRLF/multiline");
        int finishes = 0; std::string output;
        http::ResponseStream response([&](const std::string& value) { output += value; }, [&] { ++finishes; });
        check(response.send("delta", "{\"text\":\"你好\"}"), "stream send");
        response.finish(); response.finish();
        check(finishes == 1 && !response.send("done", "{}"), "stream closes once");
        StrategyFactory::instance().registerStrategy("test", [] { return std::make_shared<FakeStrategy>(); });
        int persisted = 0;
        AIHelper helper([&](int, const std::string&, bool, const std::string&, long long, const std::string&) { ++persisted; });
        try { helper.chat(1,"user","1","fail","test"); check(false,"failure expected"); } catch (const std::runtime_error&) {}
        check(helper.GetMessages().empty() && persisted == 0, "failed turn rolls back history");
        auto run = std::async(std::launch::async, [&] { return helper.chat(1,"user","1","slow","test"); });
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        auto start = Clock::now();
        check(helper.GetMessages().empty(), "uncommitted history hidden");
        check(Clock::now()-start < std::chrono::milliseconds(100), "history must not wait for generation");
        bool rejected = false;
        try { helper.chat(1,"user","1","second","test"); } catch (const std::runtime_error&) { rejected = true; }
        check(rejected, "concurrent turn rejected");
        check(run.get() == "answer" && helper.GetMessages().size() == 2 && persisted == 2, "commit complete pair");
        auto stopped = helper.chatStream(1,"user","1","stop","test", [](const std::string&) {}, [] { return true; });
        check(stopped.stopped && helper.GetMessages().size() == 4, "cancelled turn preserves role order");
        std::cout << "core tests passed\n";
        return 0;
    } catch (const ChatCancelled&) { std::cout << "cancelled\n"; return 2; }
      catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
