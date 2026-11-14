#include "AsyncSystem.hpp"

#include <boost/program_options.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/loop.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/table.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

namespace po = boost::program_options;

namespace {

template<class... Ts>
struct Overloaded : Ts...
{
    using Ts::operator()...;
};

template<class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

constexpr std::size_t max_log_entries = 200;

struct Options
{
    std::size_t cores;
    std::size_t homes;
    std::string strategy;
    std::string trafficOutputFile;
    std::string replayFile;
    std::size_t replayIntervalMs;
};

std::optional<Options> parseOptions(int argc, char** argv)
{
    Options options;
    po::options_description description("Interactive MESI simulator");
    description.add_options()
        ("help,h", "show this help")
        ("cores,c", po::value(&options.cores)->default_value(4), "number of cores")
        ("homes,m", po::value(&options.homes)->default_value(1), "number of home controllers")
        ("strategy,s", po::value(&options.strategy)->default_value("interactive"),
            "message dispatch strategy: fifo-delayed, delayed-grant or interactive")
        ("traffic-output-file,o", po::value(&options.trafficOutputFile)->required(),
            "oracle log file for message bus traffic")
        ("replay,r", po::value(&options.replayFile),
            "command file to load and start replaying on launch")
        ("replay-interval-ms,i", po::value(&options.replayIntervalMs)->default_value(100),
            "wall-clock milliseconds per simulated tick during replay");

    po::variables_map variables;
    po::store(po::parse_command_line(argc, argv, description), variables);
    if (variables.count("help")) {
        std::cout << description << '\n';
        return std::nullopt;
    }
    po::notify(variables);
    return options;
}

std::unique_ptr<mesi::IOrderingStrategy> makeOrderingStrategy(const std::string& strategy)
{
    if (strategy == "fifo-delayed") return std::make_unique<mesi::FifoDelayedOrderingStrategy>();
    if (strategy == "delayed-grant") return std::make_unique<mesi::DelayedGrantOrderingStrategy>();
    if (strategy == "interactive") return nullptr;
    throw std::invalid_argument("unknown strategy: " + strategy);
}

std::uint64_t parseNumber(const std::string& text)
{
    if (text.empty() || text[0] == '-') throw std::invalid_argument("expected unsigned number");
    std::size_t used = 0;
    const std::uint64_t n = std::stoull(text, &used, 0);
    if (used != text.size()) throw std::invalid_argument("bad number: " + text);
    return n;
}

enum class CommandKind { Load, Store, Evict };

struct CommandAction
{
    CommandKind  kind;
    mesi::Core    core;
    mesi::Address address;
    mesi::Value   value{0};
};

struct DispatchAction
{
    mesi::OracleToken token;
};

using Action = std::variant<CommandAction, DispatchAction>;

// One line of a command file: "[TICK] CORE R|W|E ADDRESS [VALUE]" or "[TICK] D TOKEN".
// The tick is optional so that traces/*.trace files load as-is.
struct ScriptEntry
{
    std::optional<std::uint64_t> tick;
    Action action;
};

std::string hex(std::uint64_t value, int width = 0)
{
    std::ostringstream out;
    out << "0x" << std::hex << std::setfill('0') << std::setw(width) << value;
    return out.str();
}

mesi::Value wordAt(const mesi::Data& data, std::size_t word)
{
    // little endian, matching AsyncSystem's load/store byte order
    mesi::Value value = 0;
    for (std::size_t byte = 0; byte < mesi::bytes_per_word; ++byte) {
        value |= mesi::Value{data[word * mesi::bytes_per_word + byte]} << (mesi::bits_in_byte * byte);
    }
    return value;
}

std::string formatAction(const Action& action)
{
    return std::visit(Overloaded{
        [](const CommandAction& command) {
            static constexpr char ops[] = {'R', 'W', 'E'};
            std::string line = std::to_string(command.core) + " " + ops[static_cast<int>(command.kind)]
                             + " " + hex(command.address);
            if (command.kind == CommandKind::Store) line += " " + hex(command.value);
            return line;
        },
        [](const DispatchAction& dispatch) { return "D " + std::to_string(dispatch.token); },
    }, action);
}

Action parseAction(const std::vector<std::string>& fields)
{
    if (fields.size() == 2 && fields[0] == "D") return DispatchAction{.token = parseNumber(fields[1])};

    if (fields.size() < 3 || fields[1].size() != 1) throw std::invalid_argument("expected CORE R|W|E ADDRESS");
    CommandAction command{.core = parseNumber(fields[0]), .address = parseNumber(fields[2])};
    switch (fields[1][0]) {
    case 'R': command.kind = CommandKind::Load; break;
    case 'W': command.kind = CommandKind::Store; break;
    case 'E': command.kind = CommandKind::Evict; break;
    default: throw std::invalid_argument("unknown operation: " + fields[1]);
    }
    const std::size_t expectedFields = command.kind == CommandKind::Store ? 4 : 3;
    if (fields.size() != expectedFields) throw std::invalid_argument("wrong number of fields");
    if (command.kind == CommandKind::Store) command.value = parseNumber(fields[3]);
    return command;
}

std::vector<ScriptEntry> loadScript(const std::string& path)
{
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot open " + path);

    std::vector<ScriptEntry> entries;
    std::string line;
    for (std::size_t row = 1; std::getline(input, line); ++row) {
        if (const auto comment = line.find('#'); comment != std::string::npos) line.resize(comment);
        std::istringstream stream(line);
        std::vector<std::string> fields;
        for (std::string field; stream >> field;) fields.push_back(field);
        if (fields.empty()) continue;

        try {
            // untimed lines start with the action itself: "D TOKEN" or "CORE R|W|E ..."
            const bool untimed = fields[0] == "D"
                || (fields.size() > 1 && (fields[1] == "R" || fields[1] == "W" || fields[1] == "E"));
            ScriptEntry entry{.action = DispatchAction{}};
            if (!untimed) {
                entry.tick = parseNumber(fields[0]);
                fields.erase(fields.begin());
            }
            entry.action = parseAction(fields);
            entries.push_back(std::move(entry));
        } catch (const std::exception& e) {
            throw std::runtime_error(path + ":" + std::to_string(row) + ": " + e.what());
        }
    }
    return entries;
}

std::string endpoint(mesi::ControllerType type, std::size_t id)
{
    return (type == mesi::ControllerType::CACHE ? "C" : "H") + std::to_string(id);
}

struct MessageSummary
{
    std::string type;
    std::string route;
    std::string line;
};

MessageSummary summarize(const mesi::Message& message)
{
    const auto make = [](const char* type, const mesi::messages::MessageHeader& header, mesi::Address line) {
        return MessageSummary{
            .type = type,
            .route = endpoint(header.sourceType, header.sourceId) + " -> "
                   + endpoint(header.destinationType, header.destinationId),
            .line = hex(line),
        };
    };
    const auto visitor = Overloaded{
        [&](const mesi::messages::GetS& m)     { return make("GetS", m.header, m.line); },
        [&](const mesi::messages::GetM& m)     { return make("GetM", m.header, m.line); },
        [&](const mesi::messages::ProbeAck& m) { return make("ProbeAck", m.header, m.echoLine); },
        [&](const mesi::messages::EvictReq& m) { return make("EvictReq", m.header, m.line); },
        [&](const mesi::messages::Grant& m)    {
            return make(m.accessType == mesi::AccessType::READ ? "Grant(R)" : "Grant(W)", m.header, m.line);
        },
        [&](const mesi::messages::ProbeS& m)   { return make("ProbeS", m.header, m.line); },
        [&](const mesi::messages::ProbeI& m)   { return make("ProbeI", m.header, m.line); },
        [&](const mesi::messages::EvictAck& m) { return make("EvictAck", m.header, m.line); },
    };
    return std::visit([&](const auto& bound) { return std::visit(visitor, bound); }, message);
}

// bold header with a rule beneath it; bodyGroupSize > 0 also draws a rule between each group of body rows
template <typename CellT>
ftxui::Element renderTable(std::vector<std::vector<CellT>> rows, std::size_t bodyGroupSize = 0)
{
    const std::size_t bodyRows = rows.size() - 1;
    ftxui::Table table(std::move(rows));
    table.SelectAll().SeparatorVertical(ftxui::EMPTY);
    table.SelectRow(0).Decorate(ftxui::bold);
    table.SelectRow(0).BorderBottom(ftxui::LIGHT);
    if (bodyGroupSize > 0) {
        for (std::size_t row = 1 + bodyGroupSize; row <= bodyRows; row += bodyGroupSize) {
            table.SelectRow(static_cast<int>(row)).BorderTop(ftxui::LIGHT);
        }
    }
    return table.Render();
}

ftxui::Component labeledInput(const std::string& label, std::string* content)
{
    auto input = ftxui::Input(content, "");
    return ftxui::Renderer(input, [label, input] {
        return ftxui::hbox({ftxui::text(label) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 9), input->Render()});
    });
}

class InteractiveSimulator
{
public:
    explicit InteractiveSimulator(const Options& options)
        : system_{options.trafficOutputFile, options.cores, options.homes, makeOrderingStrategy(options.strategy)}
        , interactive_{options.strategy == "interactive"}
        , replayInterval_{std::chrono::milliseconds(options.replayIntervalMs)}
    {
        completionHandler_ = [this](mesi::CommandTokenT token, const mesi::CommandTraitsT::results_union_t& result) {
            std::string entry = commandDescriptions_.at(token) + " done";
            if (const auto* load = std::get_if<mesi::LoadCommandTraits::ResultT>(&result)) {
                entry += " -> " + hex(load->value);
            }
            commandDescriptions_.erase(token);
            log(entry);
        };
        system_.setCompletionHandler(completionHandler_);

        if (!options.replayFile.empty()) {
            replayPath_ = options.replayFile;
            loadReplay();
            setReplayPlaying(true);
        }
    }

    void run()
    {
        auto screen = ftxui::ScreenInteractive::Fullscreen();

        auto stepForm = ftxui::Container::Vertical({
            labeledInput("ticks", &stepCount_),
            ftxui::Container::Horizontal({
                ftxui::Button("Step 1", [this] { guarded([this] { step(1); }); }),
                ftxui::Button("Step N", [this] { guarded([this] { step(parseNumber(stepCount_)); }); }),
            }),
        });

        auto commandForm = ftxui::Container::Vertical({
            ftxui::Toggle(&commandKinds_, &commandKind_),
            labeledInput("core", &commandCore_),
            labeledInput("address", &commandAddress_),
            ftxui::Maybe(labeledInput("value", &commandValue_), [this] { return isStoreSelected(); }),
            ftxui::Button("Submit", [this] { guarded([this] { submitCommand(); }); }),
        });

        auto replayForm = ftxui::Container::Vertical({
            labeledInput("file", &replayPath_),
            ftxui::Container::Horizontal({
                ftxui::Button("Load", [this] { guarded([this] { loadReplay(); }); }),
                ftxui::Button(&replayPlayLabel_, [this] { setReplayPlaying(!replayPlaying_); }),
                ftxui::Button("Next", [this] { guarded([this] { replayNextEvent(); }); }),
            }),
        });

        auto saveForm = ftxui::Container::Vertical({
            labeledInput("file", &savePath_),
            ftxui::Button("Save", [this] { guarded([this] { saveHistory(); }); }),
        });

        auto quitButton = ftxui::Button("Quit", screen.ExitLoopClosure());

        auto controls = ftxui::Container::Horizontal({
            stepForm,
            commandForm,
            replayForm,
            saveForm,
            quitButton,
        });

        auto root = ftxui::Renderer(controls, [=, this] {
            using namespace ftxui;
            auto panel = [](const std::string& title, Element body) {
                return window(text(" " + title + " ") | bold, std::move(body));
            };

            Elements forms{
                panel("Step", stepForm->Render()) | size(WIDTH, EQUAL, 24),
            };
            forms.push_back(panel("Command", commandForm->Render()) | size(WIDTH, EQUAL, 32));
            forms.push_back(panel("Replay", vbox({
                replayForm->Render(),
                text(replayProgress()),
            })) | size(WIDTH, EQUAL, 34));
            forms.push_back(panel("Save", vbox({
                saveForm->Render(),
                text(std::to_string(history_.size()) + " action(s) recorded"),
            })) | size(WIDTH, EQUAL, 34));
            forms.push_back(quitButton->Render());

            return vbox({
                hbox({
                    text(" MESI interactive simulator ") | bold,
                    filler(),
                    text("tick " + std::to_string(tick_) + " "),
                }) | inverted,
                panel("Caches", renderCaches() | vscroll_indicator | frame) | flex,
                hbox({
                    panel("Message bus", renderMessages() | vscroll_indicator | frame) | flex,
                    panel("Log", renderLog() | focusPositionRelative(0, 1) | vscroll_indicator | frame) | flex,
                }) | size(HEIGHT, EQUAL, 14),
                hbox(std::move(forms)),
                text(status_) | color(statusIsError_ ? Color::Red : Color::Green),
            });
        });

        ftxui::Loop loop(&screen, ftxui::CatchEvent(root, [this](const ftxui::Event& event) {
            return onDispatchClick(event);
        }));

        // drive replay from the UI thread so the simulator is never touched concurrently
        auto nextReplayTick = std::chrono::steady_clock::now();
        while (!loop.HasQuitted()) {
            loop.RunOnce();
            const auto now = std::chrono::steady_clock::now();
            if (replayPlaying_ && now >= nextReplayTick) {
                nextReplayTick = now + replayInterval_;
                replayTick();
                screen.PostEvent(ftxui::Event::Custom);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

private:
    void guarded(const std::function<void()>& action)
    {
        try {
            action();
        } catch (const std::exception& e) {
            setStatus(e.what(), true);
        }
    }

    void setStatus(std::string status, bool isError = false)
    {
        status_ = std::move(status);
        statusIsError_ = isError;
    }

    void log(std::string entry)
    {
        log_.push_back("[" + std::to_string(tick_) + "] " + std::move(entry));
        if (log_.size() > max_log_entries) log_.pop_front();
    }

    void step(std::uint64_t ticks)
    {
        for (std::uint64_t i = 0; i < ticks; ++i) {
            system_.step();
            ++tick_;
        }
        setStatus("stepped " + std::to_string(ticks) + " tick(s)");
    }

    bool onDispatchClick(ftxui::Event event)
    {
        if (!event.is_mouse()) return false;
        const ftxui::Mouse& mouse = event.mouse();
        if (mouse.button != ftxui::Mouse::Left || mouse.motion != ftxui::Mouse::Pressed) return false;

        for (const DispatchTarget& target : dispatchTargets_) {
            if (target.icon.Contain(mouse.x, mouse.y)) {
                guarded([this, token = target.token] { perform(DispatchAction{.token = token}); });
                return true;
            }
        }
        return false;
    }

    void dispatch(mesi::OracleToken token)
    {
        auto& bus = system_.interactiveMessageBus();
        const auto it = bus.pendingMessages().find(token);
        if (it == bus.pendingMessages().end()) {
            throw std::invalid_argument("no pending message with oracle token " + std::to_string(token));
        }
        const MessageSummary summary = summarize(it->second);
        bus.dispatchOracleToken(token);
        log("dispatched #" + std::to_string(token) + " " + summary.type + " " + summary.route);
        setStatus("dispatched oracle token " + std::to_string(token));
    }

    bool isStoreSelected() const { return commandKind_ == static_cast<int>(CommandKind::Store); }

    void submitCommand()
    {
        CommandAction command{
            .kind = static_cast<CommandKind>(commandKind_),
            .core = parseNumber(commandCore_),
            .address = parseNumber(commandAddress_),
        };
        if (command.kind == CommandKind::Store) command.value = parseNumber(commandValue_);
        perform(command);
    }

    // every user or replayed action goes through here so that it is recorded for Save
    void perform(const Action& action)
    {
        std::visit(Overloaded{
            [this](const CommandAction& command) { submit(command); },
            [this](const DispatchAction& dispatchAction) { dispatch(dispatchAction.token); },
        }, action);
        history_.push_back(ScriptEntry{.tick = tick_, .action = action});
    }

    void submit(const CommandAction& command)
    {
        if (command.core >= system_.cores()) {
            throw std::out_of_range("core out of range: " + std::to_string(command.core));
        }

        std::string description = "C" + std::to_string(command.core) + " "
                                + commandKinds_[static_cast<int>(command.kind)] + " " + hex(command.address);
        mesi::CommandTokenT token;
        switch (command.kind) {
        case CommandKind::Load:
            token = system_.submitLoadCommand(command.core, {.wordByteAddress = command.address});
            trackWord(command.address);
            break;
        case CommandKind::Store:
            token = system_.submitStoreCommand(command.core, {.wordByteAddress = command.address, .value = command.value});
            trackWord(command.address);
            description += " = " + hex(command.value);
            break;
        case CommandKind::Evict:
            token = system_.submitEvictCommand(command.core, {.cacheLineAddress = command.address});
            trackedLines_.insert(mesi::line_base(command.address));
            break;
        }

        description = "cmd " + std::to_string(token) + ": " + description;
        commandDescriptions_.emplace(token, description);
        log(description + " submitted");
        setStatus("submitted " + description);
    }

    void saveHistory()
    {
        std::ofstream output(savePath_);
        if (!output) throw std::runtime_error("cannot write " + savePath_);
        output << "# tick action\n";
        for (const ScriptEntry& entry : history_) {
            output << *entry.tick << ' ' << formatAction(entry.action) << '\n';
        }
        setStatus("saved " + std::to_string(history_.size()) + " action(s) to " + savePath_);
    }

    void loadReplay()
    {
        const std::vector<ScriptEntry> entries = loadScript(replayPath_);

        // keep the recorded spacing, starting from the current tick; an untimed entry
        // (e.g. from a trace file) fires on the same tick as the entry before it
        replay_.clear();
        const std::uint64_t firstTick = entries.empty() || !entries.front().tick ? 0 : *entries.front().tick;
        std::uint64_t offset = 0;
        for (const ScriptEntry& entry : entries) {
            if (entry.tick) {
                if (*entry.tick < firstTick + offset) throw std::runtime_error("ticks in " + replayPath_ + " go backwards");
                offset = *entry.tick - firstTick;
            }
            replay_.push_back(ScheduledAction{.tick = tick_ + offset, .action = entry.action});
        }
        replayNext_ = 0;
        setReplayPlaying(false);
        log("loaded " + std::to_string(replay_.size()) + " action(s) from " + replayPath_);
        setStatus("loaded " + replayPath_);
    }

    void setReplayPlaying(bool playing)
    {
        replayPlaying_ = playing && replayNext_ < replay_.size();
        replayPlayLabel_ = replayPlaying_ ? "Pause" : "Play";
    }

    bool replayFinished() const { return replayNext_ >= replay_.size(); }

    // perform every replay action scheduled at or before the current tick
    void fireDueReplayActions()
    {
        while (!replayFinished() && replay_[replayNext_].tick <= tick_) {
            const Action& action = replay_[replayNext_].action;
            try {
                perform(action);
            } catch (const std::exception& e) {
                setReplayPlaying(false);
                throw std::runtime_error("replay stopped at \"" + formatAction(action) + "\": " + e.what());
            }
            ++replayNext_;
        }
        if (replayFinished()) setReplayPlaying(false);
    }

    void replayTick()
    {
        guarded([this] {
            fireDueReplayActions();
            if (!replayFinished()) step(1);
            setStatus(replayFinished() ? "replay finished" : replayProgress());
        });
    }

    void replayNextEvent()
    {
        if (replayFinished()) throw std::logic_error("no replay actions left");
        const std::uint64_t due = replay_[replayNext_].tick;
        if (due > tick_) step(due - tick_);
        fireDueReplayActions();
        setStatus(replayFinished() ? "replay finished" : replayProgress());
    }

    std::string replayProgress() const
    {
        if (replay_.empty()) return "nothing loaded";
        std::string progress = std::to_string(replayNext_) + "/" + std::to_string(replay_.size()) + " done";
        if (!replayFinished()) progress += ", next at tick " + std::to_string(replay_[replayNext_].tick);
        return progress;
    }

    void trackWord(mesi::Address address)
    {
        // a word may straddle two cache lines
        trackedLines_.insert(mesi::line_base(address));
        trackedLines_.insert(mesi::line_base(address + mesi::bytes_per_word - 1));
    }

    ftxui::Element renderCaches() const
    {
        using namespace ftxui;
        if (trackedLines_.empty()) return text("no lines touched yet");

        std::vector<std::vector<std::string>> rows{{"Line", "Core", "State"}};
        for (std::size_t word = 0; word < mesi::words_per_line; ++word) {
            rows[0].push_back("Word " + std::to_string(word));
        }
        for (const mesi::Address line : trackedLines_) {
            for (mesi::Core core = 0; core < system_.cores(); ++core) {
                const mesi::CacheLine cacheLine = system_.inspect(core, line);
                const bool valid = cacheLine.state != mesi::State::I;
                std::vector<std::string> row{
                    core == 0 ? hex(line) : "",
                    "C" + std::to_string(core),
                    std::string(1, mesi::letter(cacheLine.state)),
                };
                for (std::size_t word = 0; word < mesi::words_per_line; ++word) {
                    row.push_back(hex(valid ? wordAt(cacheLine.data, word) : 0, 16));
                }
                rows.push_back(std::move(row));
            }
        }

        return renderTable(std::move(rows), system_.cores());
    }

    ftxui::Element renderMessages()
    {
        using namespace ftxui;
        dispatchTargets_.clear();
        if (!interactive_) return text("messages are dispatched automatically by the ordering strategy");

        const auto& pending = system_.interactiveMessageBus().pendingMessages();
        if (pending.empty()) return text("no pending messages");

        // sized up front: reflect() keeps a reference to each icon's Box until the frame is drawn
        dispatchTargets_.resize(pending.size());
        std::vector<std::vector<Element>> rows{
            {text(""), text("Token"), text("Type"), text("Route"), text("Line")},
        };
        auto target = dispatchTargets_.begin();
        for (const auto& [token, message] : pending) {
            const MessageSummary summary = summarize(message);
            target->token = token;
            rows.push_back({
                text("▶") | color(Color::Green) | reflect(target->icon),
                text(std::to_string(token)),
                text(summary.type),
                text(summary.route),
                text(summary.line),
            });
            ++target;
        }

        return renderTable(std::move(rows));
    }

    ftxui::Element renderLog() const
    {
        using namespace ftxui;
        Elements lines;
        for (const std::string& entry : log_) {
            lines.push_back(text(entry));
        }
        return vbox(std::move(lines));
    }

    struct ScheduledAction
    {
        std::uint64_t tick;
        Action action;
    };

    struct DispatchTarget
    {
        mesi::OracleToken token;
        ftxui::Box icon;
    };

    mesi::AsyncSystem system_;
    bool interactive_;
    std::chrono::milliseconds replayInterval_;
    mesi::AsyncSystem::CompletionHandlerT completionHandler_;
    std::uint64_t tick_{0};

    std::set<mesi::Address> trackedLines_;
    std::unordered_map<mesi::CommandTokenT, std::string> commandDescriptions_;
    std::deque<std::string> log_;
    std::string status_;
    bool statusIsError_{false};
    std::vector<DispatchTarget> dispatchTargets_;

    std::string stepCount_{"10"};
    std::vector<std::string> commandKinds_{"Load", "Store", "Evict"};
    int commandKind_{0};
    std::string commandCore_{"0"};
    std::string commandAddress_;
    std::string commandValue_;

    std::vector<ScriptEntry> history_;
    std::string savePath_{"commands.txt"};

    std::vector<ScheduledAction> replay_;
    std::size_t replayNext_{0};
    bool replayPlaying_{false};
    std::string replayPlayLabel_{"Play"};
    std::string replayPath_{"commands.txt"};
};

} // namespace

int main(int argc, char** argv)
{
    try {
        const std::optional<Options> options = parseOptions(argc, argv);
        if (!options) return 0;
        InteractiveSimulator simulator(*options);
        simulator.run();
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
