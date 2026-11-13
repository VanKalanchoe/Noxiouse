#include "Console.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <format>

#include "NoxCore/Core/Log.h"

namespace Nox
{
    namespace
    {
        std::string Lower(std::string_view text)
        {
            std::string lower(text);
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return lower;
        }

        void LogLine(ConsoleLevel level, const std::string& text)
        {
            if (!Log::GetCoreLogger())
                return;
            switch (level)
            {
            case ConsoleLevel::Info: NOX_CORE_INFO("{}", text); break;
            case ConsoleLevel::Warning: NOX_CORE_WARN("{}", text); break;
            case ConsoleLevel::Error: NOX_CORE_ERROR("{}", text); break;
            }
        }

        bool ParseBool(std::string_view text, bool& value)
        {
            const std::string lower = Lower(text);
            if (lower == "1" || lower == "true" || lower == "on" || lower == "yes") { value = true; return true; }
            if (lower == "0" || lower == "false" || lower == "off" || lower == "no") { value = false; return true; }
            return false;
        }

        std::string FormatFloat(float value)
        {
            return std::format("{:.6g}", value);
        }

        std::string ValueText(int32_t value) { return std::to_string(value); }
        std::string ValueText(float value) { return FormatFloat(value); }

        const char* TypeName(ConsoleVariable::Type type)
        {
            switch (type)
            {
            case ConsoleVariable::Type::Bool: return "bool";
            case ConsoleVariable::Type::Int: return "int";
            case ConsoleVariable::Type::Float: return "float";
            case ConsoleVariable::Type::String: return "string";
            }
            return "?";
        }
    }

    const char* ToString(ConsoleAccess access)
    {
        switch (access)
        {
        case ConsoleAccess::Read: return "read";
        case ConsoleAccess::Write: return "write";
        case ConsoleAccess::Execute: return "execute";
        case ConsoleAccess::User: return "user";
        }
        return "?";
    }

    // ------------------------------------------------------------------------------------------------------------------ output

    bool ConsoleOutput::HasErrors() const
    {
        return std::any_of(Lines.begin(), Lines.end(), [](const Line& line) { return line.Level == ConsoleLevel::Error; });
    }

    std::string ConsoleOutput::Text() const
    {
        std::string text;
        for (const Line& line : Lines)
        {
            if (!text.empty())
                text += '\n';
            text += line.Text;
        }
        return text;
    }

    // --------------------------------------------------------------------------------------------------------------- variables

    ConsoleVariable::ConsoleVariable(std::string name, std::string help, Type type)
        : m_Name(std::move(name)), m_Help(std::move(help)), m_Type(type)
    {
    }

    void ConsoleVariable::NotifyChanged()
    {
        for (const auto& callback : m_Callbacks)
            callback(*this);
    }

    template<typename T>
    static constexpr ConsoleVariable::Type TypeOf()
    {
        if constexpr (std::is_same_v<T, bool>) return ConsoleVariable::Type::Bool;
        else if constexpr (std::is_same_v<T, int32_t>) return ConsoleVariable::Type::Int;
        else if constexpr (std::is_same_v<T, float>) return ConsoleVariable::Type::Float;
        else return ConsoleVariable::Type::String;
    }

    template<typename T>
    TConsoleVariable<T>::TConsoleVariable(std::string name, std::string help, T value, T minimum, T maximum, bool ranged)
        : ConsoleVariable(std::move(name), std::move(help), TypeOf<T>()), m_Value(value), m_Default(value), m_Min(minimum), m_Max(maximum),
          m_Ranged(ranged)
    {
    }

    template<typename T>
    void TConsoleVariable<T>::Set(T value)
    {
        if constexpr (std::is_same_v<T, int32_t> || std::is_same_v<T, float>)
            if (m_Ranged)
                value = std::clamp(value, m_Min, m_Max);
        if (value == m_Value)
            return;
        m_Value = value;
        NotifyChanged();
    }

    template<typename T>
    std::string TConsoleVariable<T>::GetString() const
    {
        if constexpr (std::is_same_v<T, bool>) return m_Value ? "1" : "0";
        else if constexpr (std::is_same_v<T, int32_t>) return std::to_string(m_Value);
        else if constexpr (std::is_same_v<T, float>) return FormatFloat(m_Value);
        else return m_Value;
    }

    template<typename T>
    std::string TConsoleVariable<T>::GetDefaultString() const
    {
        if constexpr (std::is_same_v<T, bool>) return m_Default ? "1" : "0";
        else if constexpr (std::is_same_v<T, int32_t>) return std::to_string(m_Default);
        else if constexpr (std::is_same_v<T, float>) return FormatFloat(m_Default);
        else return m_Default;
    }

    template<typename T>
    std::string TConsoleVariable<T>::DescribeRange() const
    {
        if constexpr (std::is_same_v<T, int32_t>)
            return m_Ranged ? std::format("{}..{}", m_Min, m_Max) : std::string();
        else if constexpr (std::is_same_v<T, float>)
            return m_Ranged ? std::format("{}..{}", FormatFloat(m_Min), FormatFloat(m_Max)) : std::string();
        else if constexpr (std::is_same_v<T, bool>)
            return "0 / 1";
        else
            return {};
    }

    template<typename T>
    bool TConsoleVariable<T>::SetString(std::string_view text, std::string& error)
    {
        T value{};
        if constexpr (std::is_same_v<T, bool>)
        {
            if (!ParseBool(text, value))
            {
                error = std::format("'{}' is not a bool (0 / 1, on / off, true / false)", text);
                return false;
            }
        }
        else if constexpr (std::is_same_v<T, int32_t>)
        {
            const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
            if (result.ec != std::errc() || result.ptr != text.data() + text.size())
            {
                error = std::format("'{}' is not a whole number", text);
                return false;
            }
        }
        else if constexpr (std::is_same_v<T, float>)
        {
            // (std::from_chars for float is not everywhere yet: strtof on a terminated copy, the whole text must be read)
            const std::string copy(text);
            char* end = nullptr;
            value = std::strtof(copy.c_str(), &end);
            if (copy.empty() || end != copy.c_str() + copy.size() || !std::isfinite(value))
            {
                error = std::format("'{}' is not a number", text);
                return false;
            }
        }
        else
            value = std::string(text);

        if constexpr (std::is_same_v<T, int32_t> || std::is_same_v<T, float>)
        {
            if (m_Ranged && (value < m_Min || value > m_Max))
            {
                error = std::format("{} is outside {}..{}", text, ValueText(m_Min), ValueText(m_Max));
                return false;
            }
        }
        Set(value);
        return true;
    }

    // ------------------------------------------------------------------------------------------------------------ registration

    Console::Registration& Console::Registration::operator=(Registration&& other) noexcept
    {
        if (this != &other)
        {
            Reset();
            m_Name = std::move(other.m_Name);
            m_Id = other.m_Id;
            other.m_Name.clear();
        }
        return *this;
    }

    void Console::Registration::Reset()
    {
        if (!m_Name.empty())
            Console::Get().UnregisterCommand(m_Name, m_Id);
        m_Name.clear();
    }

    // ----------------------------------------------------------------------------------------------------------------- console

    Console& Console::Get()
    {
        static Console console;
        return console;
    }

    Console::Console()
    {
        RegisterBuiltins();
    }

    Console::Registration Console::RegisterCommand(CommandInfo command)
    {
        const std::string name = command.Name;
        const uint64_t id = AddCommand(std::move(command));
        return Registration(name, id);
    }

    Console::Registration Console::RegisterAlias(std::string name, std::string target, std::string usage)
    {
        CommandInfo alias;
        alias.Name = std::move(name);
        alias.Usage = std::move(usage);
        alias.Help = std::format("Same as {}", target);
        alias.Access = ConsoleAccess::Read; // (the target's own access is checked when it runs)
        alias.AliasOf = std::move(target);
        return RegisterCommand(std::move(alias));
    }

    uint64_t Console::AddCommand(CommandInfo command)
    {
        const std::string key = Lower(command.Name);
        const uint64_t id = m_NextId++;
        m_Commands[key] = StoredCommand{ std::move(command), id };
        return id;
    }

    void Console::UnregisterCommand(std::string_view name, uint64_t id)
    {
        auto found = m_Commands.find(Lower(name));
        if (found != m_Commands.end() && found->second.Id == id)
            m_Commands.erase(found);
    }

    void Console::AddVariableBase(std::unique_ptr<ConsoleVariable> variable)
    {
        m_Variables[Lower(variable->GetName())] = std::move(variable);
    }

    void Console::Unregister(std::string_view name)
    {
        const std::string key = Lower(name);
        m_Commands.erase(key);
        m_Variables.erase(key);
    }

    ConsoleVariable* Console::FindVariable(std::string_view name) const
    {
        auto found = m_Variables.find(Lower(name));
        return found != m_Variables.end() ? found->second.get() : nullptr;
    }

    const Console::CommandInfo* Console::FindCommand(std::string_view name) const
    {
        auto found = m_Commands.find(Lower(name));
        return found != m_Commands.end() ? &found->second.Info : nullptr;
    }

    std::vector<std::string> Console::Split(std::string_view line)
    {
        std::vector<std::string> words;
        std::string word;
        bool quoted = false, inWord = false;
        for (char c : line)
        {
            if (c == '"')
            {
                quoted = !quoted;
                inWord = true; // ("" is an empty word)
                continue;
            }
            if (!quoted && std::isspace(static_cast<unsigned char>(c)))
            {
                if (inWord)
                    words.push_back(std::move(word));
                word.clear();
                inWord = false;
                continue;
            }
            word += c;
            inWord = true;
        }
        if (inWord)
            words.push_back(std::move(word));
        return words;
    }

    ConsoleOutput Console::Execute(std::string_view line, ConsoleAccess maxAccess)
    {
        ConsoleOutput output;
        // Commands separated by ';' (outside quotes).
        std::vector<std::string_view> commands;
        size_t start = 0;
        bool quoted = false;
        for (size_t index = 0; index <= line.size(); ++index)
        {
            if (index < line.size() && line[index] == '"')
                quoted = !quoted;
            if (index == line.size() || (line[index] == ';' && !quoted))
            {
                commands.push_back(line.substr(start, index - start));
                start = index + 1;
            }
        }
        for (std::string_view command : commands)
        {
            while (!command.empty() && std::isspace(static_cast<unsigned char>(command.front())))
                command.remove_prefix(1);
            while (!command.empty() && std::isspace(static_cast<unsigned char>(command.back())))
                command.remove_suffix(1);
            const std::vector<std::string> words = Split(command);
            if (words.empty())
                continue;
            LogLine(ConsoleLevel::Info, std::format("> {}", command));
            const size_t first = output.Lines.size();
            RunOne(words, maxAccess, output);
            for (size_t index = first; index < output.Lines.size(); ++index)
                LogLine(output.Lines[index].Level, output.Lines[index].Text);
        }
        return output;
    }

    void Console::RunOne(const std::vector<std::string>& words, ConsoleAccess maxAccess, ConsoleOutput& output)
    {
        const std::string key = Lower(words[0]);
        if (auto command = m_Commands.find(key); command != m_Commands.end())
        {
            const CommandInfo& info = command->second.Info;
            if (!info.AliasOf.empty())
            {
                std::vector<std::string> target = words;
                target[0] = info.AliasOf;
                if (Lower(info.AliasOf) != key) // (an alias of itself would never end)
                    RunOne(target, maxAccess, output);
                return;
            }
            if (static_cast<uint8_t>(info.Access) > static_cast<uint8_t>(maxAccess))
            {
                output.Error(std::format("{}: needs {} access (granted: {})", info.Name, ToString(info.Access), ToString(maxAccess)));
                return;
            }
            const std::vector<std::string> arguments(words.begin() + 1, words.end());
            // (copied: a command may register or remove commands, the map entry could go away while it runs)
            const ConsoleHandler handler = info.Handler;
            if (handler)
                handler(arguments, output);
            return;
        }
        if (auto variable = m_Variables.find(key); variable != m_Variables.end())
        {
            ConsoleVariable& cvar = *variable->second;
            if (words.size() == 1)
            {
                const std::string range = cvar.DescribeRange();
                output.Info(std::format("{} = {}  ({}{}{}, default {}){}{}", cvar.GetName(), cvar.GetString(), TypeName(cvar.GetType()),
                                        range.empty() ? "" : " ", range, cvar.GetDefaultString(), cvar.GetHelp().empty() ? "" : " -- ",
                                        cvar.GetHelp()));
                return;
            }
            if (static_cast<uint8_t>(maxAccess) < static_cast<uint8_t>(cvar.GetRequiredAccess()))
            {
                output.Error(std::format("{}: setting it needs {} access (granted: {})", cvar.GetName(), ToString(cvar.GetRequiredAccess()),
                                         ToString(maxAccess)));
                return;
            }
            // (a string variable takes the rest of the line)
            std::string value = words[1];
            if (cvar.GetType() == ConsoleVariable::Type::String)
                for (size_t index = 2; index < words.size(); ++index)
                    value += " " + words[index];
            std::string error;
            if (!cvar.SetString(value, error))
                output.Error(std::format("{}: {}", cvar.GetName(), error));
            else
                output.Info(std::format("{} = {}", cvar.GetName(), cvar.GetString()));
            return;
        }
        // Unknown: the closest names help with typos.
        std::string suggestion;
        for (const std::string& name : Complete(words[0].substr(0, std::min<size_t>(words[0].size(), 3))))
        {
            suggestion += suggestion.empty() ? " (did you mean: " : ", ";
            suggestion += name;
            if (suggestion.size() > 120)
                break;
        }
        output.Error(std::format("Unknown command or variable '{}'{}{} -- 'help' lists them", words[0], suggestion, suggestion.empty() ? "" : ")"));
    }

    std::vector<std::string> Console::Complete(std::string_view prefix) const
    {
        const std::string lower = Lower(prefix);
        std::vector<std::string> names;
        for (const auto& [key, command] : m_Commands)
            if (key.compare(0, lower.size(), lower) == 0)
                names.push_back(command.Info.Name);
        for (const auto& [key, variable] : m_Variables)
            if (key.compare(0, lower.size(), lower) == 0)
                names.push_back(variable->GetName());
        std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) { return Lower(a) < Lower(b); });
        return names;
    }

    std::vector<Console::Entry> Console::List(std::string_view filter) const
    {
        const std::string lower = Lower(filter);
        std::vector<Entry> entries;
        for (const auto& [key, command] : m_Commands)
        {
            if (!lower.empty() && key.find(lower) == std::string::npos && Lower(command.Info.Help).find(lower) == std::string::npos)
                continue;
            entries.push_back({ command.Info.Name, command.Info.Usage.empty() ? command.Info.Name : command.Info.Usage, command.Info.Help, false, {},
                                command.Info.Access });
        }
        for (const auto& [key, variable] : m_Variables)
        {
            if (!lower.empty() && key.find(lower) == std::string::npos && Lower(variable->GetHelp()).find(lower) == std::string::npos)
                continue;
            entries.push_back({ variable->GetName(), variable->GetName() + " [value]", variable->GetHelp(), true, variable->GetString(),
                                variable->GetRequiredAccess() });
        }
        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return Lower(a.Name) < Lower(b.Name); });
        return entries;
    }

    void Console::RegisterBuiltins()
    {
        // (built in: added without a Registration, they last as long as the console)
        AddCommand({ "help", "help [name]", "Lists the commands and variables, or describes one", ConsoleAccess::Read,
            [this](const std::vector<std::string>& arguments, ConsoleOutput& output)
            {
                if (!arguments.empty())
                {
                    if (const CommandInfo* command = FindCommand(arguments[0]))
                    {
                        output.Info(std::format("{} -- {} ({} access)", command->Usage.empty() ? command->Name : command->Usage, command->Help,
                                                ToString(command->Access)));
                        return;
                    }
                    if (FindVariable(arguments[0]))
                    {
                        RunOne({ arguments[0] }, ConsoleAccess::Read, output);
                        return;
                    }
                    output.Error(std::format("No command or variable '{}'", arguments[0]));
                    return;
                }
                for (const Entry& entry : List())
                    output.Info(entry.IsVariable ? std::format("{} = {} -- {}", entry.Name, entry.Value, entry.Help)
                                                 : std::format("{} -- {}", entry.Usage, entry.Help));
            } });
        AddCommand({ "find", "find <text>", "Lists the commands and variables whose name or help contains the text",
            ConsoleAccess::Read,
            [this](const std::vector<std::string>& arguments, ConsoleOutput& output)
            {
                if (arguments.empty())
                {
                    output.Error("find <text>");
                    return;
                }
                std::string filter = arguments[0];
                for (size_t index = 1; index < arguments.size(); ++index)
                    filter += " " + arguments[index];
                const std::vector<Entry> entries = List(filter);
                if (entries.empty())
                    output.Info(std::format("Nothing matches '{}'", filter));
                for (const Entry& entry : entries)
                    output.Info(entry.IsVariable ? std::format("{} = {} -- {}", entry.Name, entry.Value, entry.Help)
                                                 : std::format("{} -- {}", entry.Usage, entry.Help));
            } });
    }

    template class TConsoleVariable<bool>;
    template class TConsoleVariable<int32_t>;
    template class TConsoleVariable<float>;
    template class TConsoleVariable<std::string>;
}
