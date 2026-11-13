#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// The engine console (Unreal's console commands and console variables; docs/MCP_Plan_2026-11.md §4): named commands and variables
// registered by the engine, the editor and games, run from the editor's Output Log input line, the dedicated server's terminal and
// tools (MCP's run_command). Main thread only: callers on other threads queue their lines to it.
//
// A line: a name and its arguments, words split at spaces ("quoted words" stay one), several commands separated by ';'.
// A variable typed alone prints its value and help; with an argument it is set (checked against its type and range).
namespace Nox
{
    enum class ConsoleLevel : uint8_t { Info, Warning, Error };

    // What a command or variable printed (also logged): a tool reads it back, the Output Log shows it.
    struct ConsoleOutput
    {
        struct Line
        {
            ConsoleLevel Level = ConsoleLevel::Info;
            std::string Text;
        };
        std::vector<Line> Lines;
        void Info(std::string text) { Lines.push_back({ ConsoleLevel::Info, std::move(text) }); }
        void Warning(std::string text) { Lines.push_back({ ConsoleLevel::Warning, std::move(text) }); }
        void Error(std::string text) { Lines.push_back({ ConsoleLevel::Error, std::move(text) }); }
        bool HasErrors() const;
        std::string Text() const; // every line, '\n' between
    };

    // Who may run it (MCP permissions: Read, Write, Execute). A command declares what it does; an MCP client is granted some.
    enum class ConsoleAccess : uint8_t
    {
        Read,    // only looks (status, help, printing a variable)
        Write,   // changes settings or the scene
        Execute, // runs something with consequences outside the editor's state (quit, kick, build, package)
        User     // only the person at this machine (the Output Log's Cmd line, the server's terminal): never granted to a tool --
                 // e.g. the MCP server's own settings, so a client cannot raise its own access
    };

    using ConsoleHandler = std::function<void(const std::vector<std::string>& arguments, ConsoleOutput& output)>;

    class ConsoleVariable
    {
    public:
        enum class Type : uint8_t { Bool, Int, Float, String };

        ConsoleVariable(std::string name, std::string help, Type type);
        virtual ~ConsoleVariable() = default;

        const std::string& GetName() const { return m_Name; }
        const std::string& GetHelp() const { return m_Help; }
        Type GetType() const { return m_Type; }
        virtual std::string GetString() const = 0;
        virtual std::string GetDefaultString() const = 0;
        virtual std::string DescribeRange() const { return {}; } // "0..3", empty when any value of its type is allowed
        // Parses and sets; false (and `error`) when the text is not a value of its type or out of its range.
        virtual bool SetString(std::string_view text, std::string& error) = 0;
        // Called after every change (a setting applied to the running engine).
        void OnChanged(std::function<void(ConsoleVariable&)> callback) { m_Callbacks.push_back(std::move(callback)); }
        // The access setting it needs from the console (Write by default; User: only the person at this machine).
        void SetRequiredAccess(ConsoleAccess access) { m_SetAccess = access; }
        ConsoleAccess GetRequiredAccess() const { return m_SetAccess; }

    protected:
        void NotifyChanged();

    private:
        std::string m_Name, m_Help;
        Type m_Type;
        ConsoleAccess m_SetAccess = ConsoleAccess::Write;
        std::vector<std::function<void(ConsoleVariable&)>> m_Callbacks;
    };

    // A variable of type T (bool, int32_t, float, std::string), optionally with a range (numbers).
    template<typename T>
    class TConsoleVariable final : public ConsoleVariable
    {
    public:
        TConsoleVariable(std::string name, std::string help, T value, T minimum = T(), T maximum = T(), bool ranged = false);

        T Get() const { return m_Value; }
        void Set(T value); // clamped to the range; callbacks run when it changed
        std::string GetString() const override;
        std::string GetDefaultString() const override;
        std::string DescribeRange() const override;
        bool SetString(std::string_view text, std::string& error) override;

    private:
        T m_Value, m_Default, m_Min, m_Max;
        bool m_Ranged;
    };

    class Console
    {
    public:
        struct CommandInfo
        {
            std::string Name;
            std::string Usage; // "kick <client id> [reason]"
            std::string Help;
            ConsoleAccess Access = ConsoleAccess::Write;
            ConsoleHandler Handler;
            std::string AliasOf; // set: another name for that command or variable (its arguments passed on; Handler unused)
        };
        // A registration that ends with its owner (a command capturing `this` is removed with it).
        class Registration
        {
        public:
            Registration() = default;
            Registration(std::string name, uint64_t id) : m_Name(std::move(name)), m_Id(id) {}
            Registration(Registration&& other) noexcept : m_Name(std::move(other.m_Name)), m_Id(other.m_Id) { other.m_Name.clear(); }
            Registration& operator=(Registration&& other) noexcept;
            Registration(const Registration&) = delete;
            Registration& operator=(const Registration&) = delete;
            ~Registration() { Reset(); }
            void Reset();

        private:
            std::string m_Name;
            uint64_t m_Id = 0; // (a command registered again under the name since is not removed)
        };

        static Console& Get();

        // A command (names are case-insensitive, by convention "area.Name" for engine ones: "net.Status"). Registering a name
        // again replaces it.
        Registration RegisterCommand(CommandInfo command);
        // Another name for a command or variable ("netemu" for net.Emulation); it needs the access its target needs.
        Registration RegisterAlias(std::string name, std::string target, std::string usage = {});
        // A variable it owns; the returned pointer stays valid until it is unregistered.
        template<typename T>
        TConsoleVariable<T>* RegisterVariable(std::string name, std::string help, T value)
        {
            return AddVariable<T>(std::make_unique<TConsoleVariable<T>>(std::move(name), std::move(help), value));
        }
        template<typename T>
        TConsoleVariable<T>* RegisterVariable(std::string name, std::string help, T value, T minimum, T maximum)
        {
            return AddVariable<T>(std::make_unique<TConsoleVariable<T>>(std::move(name), std::move(help), value, minimum, maximum, true));
        }
        void Unregister(std::string_view name);

        ConsoleVariable* FindVariable(std::string_view name) const;
        const CommandInfo* FindCommand(std::string_view name) const;

        // Runs a line (';' separates commands). Each command's output is logged and returned; `maxAccess` refuses commands that need
        // more (a tool passes what it was granted; the person at this machine -- the Cmd line, the server terminal -- has User).
        ConsoleOutput Execute(std::string_view line, ConsoleAccess maxAccess = ConsoleAccess::User);

        // Names starting with `prefix` (case ignored), sorted: Tab completion.
        std::vector<std::string> Complete(std::string_view prefix) const;
        struct Entry
        {
            std::string Name;
            std::string Usage;
            std::string Help;
            bool IsVariable = false;
            std::string Value; // variables
            ConsoleAccess Access = ConsoleAccess::Write;
        };
        // Every command and variable whose name or help contains `filter` (case ignored; empty: all), sorted by name.
        std::vector<Entry> List(std::string_view filter = {}) const;

        // Words of a line ("quoted words" kept together, quotes removed).
        static std::vector<std::string> Split(std::string_view line);

    private:
        Console();
        template<typename T>
        TConsoleVariable<T>* AddVariable(std::unique_ptr<TConsoleVariable<T>> variable)
        {
            TConsoleVariable<T>* raw = variable.get();
            AddVariableBase(std::move(variable));
            return raw;
        }
        void AddVariableBase(std::unique_ptr<ConsoleVariable> variable);
        void RunOne(const std::vector<std::string>& words, ConsoleAccess maxAccess, ConsoleOutput& output);
        void RegisterBuiltins();
        void UnregisterCommand(std::string_view name, uint64_t id);
        uint64_t AddCommand(CommandInfo command);

        struct StoredCommand
        {
            CommandInfo Info;
            uint64_t Id = 0;
        };
        uint64_t m_NextId = 1;

        std::map<std::string, StoredCommand> m_Commands;                       // by lower-case name
        std::map<std::string, std::unique_ptr<ConsoleVariable>> m_Variables;  // by lower-case name
    };

    const char* ToString(ConsoleAccess access);
}
