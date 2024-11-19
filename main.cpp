#include <iostream>
#include <sstream>
#include <map>
#include <csignal>
#include <chrono>
#include <condition_variable>
#include <vector>
#include <thread>
#include <atomic>
#include <functional>
#include <cmath>
#include <iterator>
#include <mutex>
#include <memory>

using namespace std;

// Тип функції завдання
using TaskFunction = function<double(int, atomic<bool>&)>;

// Завдання для виконання
struct Task {
    string name;
    TaskFunction func;
    int argument;
    int timeout_ms;
    double result = NAN;
    shared_ptr<atomic<bool>> cancel_flag; // Динамічний атомарний прапорець
};

map<string, vector<Task>> groups; // Групи з компонентами
string current_group;
mutex mtx;
condition_variable cv; // Одне з умов варіанта було використання condition-змінних
atomic<int> active_tasks = 0; // Лічильник активних завдань

// Функції обчислення
// Реалізація така, щоб ніби симулювати складне або комплексне завдання
// Імплементував, щоби x був не для групи, а для кожного компонета свій
double square(int x, atomic<bool>& flag) { // Затримка десь 1 сек
    for (int i = 1; i <= 10; ++i) {
        if (flag) {
            cout << "Square calculation cancelled.\n";
            return NAN;
        }
        this_thread::sleep_for(chrono::milliseconds(100)); // Затримка
    }
    return x * x;
}

double square_root(int x, atomic<bool>& flag) { // Затримка десь 2 сек
    for (int i = 1; i <= 20; ++i) {
        if (flag) {
            cout << "Sqrt calculation cancelled.\n";
            return NAN;
        }
        this_thread::sleep_for(chrono::milliseconds(100)); // Затримка
    }
    return sqrt(x);
}

double factorial(int x, atomic<bool>& flag) { // Затримка залежить від аргумента x
    double result = 1;
    for (int i = 1; i <= x; ++i) {
        if (flag) {
            cout << "Factorial calculation cancelled.\n";
            return NAN;
        }
        this_thread::sleep_for(chrono::milliseconds(100)); // Затримка
        result *= i;
    }
    return result;
}

// Обробка сигналу Ctrl+C
void handleCtrlC(int signal) {
    lock_guard<mutex> lock(mtx);
    for (auto& [group_name, tasks] : groups) {
        for (auto& task : tasks) {
            *task.cancel_flag = true; // Скасувати всі завдання
        }
    }
    cout << "\nAll tasks in all groups have been cancelled due to Ctrl+C.\n";
}

// Створити групу
void createGroup(const string& group_name) {
    if (groups.find(group_name) != groups.end()) {
        cerr << "Group " << group_name << " already exists.\n";
    }
    else {
        groups[group_name] = {};
        cout << "Group " << group_name << " created.\n";
    }
}

// Переключити групу
void switchGroup(const string& group_name) {
    if (groups.find(group_name) == groups.end()) {
        cerr << "Group " << group_name << " does not exist.\n";
    }
    else {
        current_group = group_name;
        cout << "Switched to group " << group_name << ".\n";
    }
}

// Додати завдання
void addTask(const string& group_name, const string& task_name, TaskFunction func, int argument, int timeout_ms) {
    if (groups.find(group_name) == groups.end()) {
        cerr << "Group " << group_name << " does not exist.\n";
        return;
    }
    auto cancel_flag = make_shared<atomic<bool>>(false); // Динамічний атомарний прапорець
    groups[group_name].push_back({ task_name, func, argument, timeout_ms, NAN, cancel_flag });
    cout << "Task " << task_name << " added to group " << group_name << ".\n";
}

// Показати статус завдань у поточній групі
void showStatus(const string& group_name) {
    if (groups.find(group_name) == groups.end()) {
        cerr << "Group " << group_name << " does not exist.\n";
        return;
    }

    cout << "Status of tasks in group " << group_name << ":\n";
    for (const auto& task : groups[group_name]) {
        cout << "  Task " << task.name << ": ";
        if (!isnan(task.result)) {
            cout << "Completed (Result = " << task.result << ")\n";
        }
        else if (isnan(task.result)) {
            cout << "Cancelled\n";
        }
        else {
            cout << "In Progress or Not Started\n";
        }
    }
}

// Зведення про всі групи
void showSummary() {
    cout << "Summary of all groups:\n";
    for (const auto& [group_name, tasks] : groups) {
        int completed = 0;
        for (const auto& task : tasks) {
            if (!isnan(task.result)) {
                ++completed;
            }
        }
        cout << "  Group " << group_name << ": " << tasks.size() << " tasks, " << completed << " completed.\n";
    }
}

// Запустити групу
void runGroup(const string& group_name) {
    if (groups.find(group_name) == groups.end()) {
        cerr << "Group " << group_name << " does not exist.\n";
        return;
    }

    vector<thread> threads;
    active_tasks = groups[group_name].size(); 

    for (auto& task : groups[group_name]) {
        threads.emplace_back([&task]() {
            auto start_time = chrono::steady_clock::now();

            thread timeout_thread([&task, start_time]() {
                while (chrono::steady_clock::now() - start_time < chrono::milliseconds(task.timeout_ms)) {
                    if (*task.cancel_flag) return; 
                    this_thread::sleep_for(chrono::milliseconds(10)); 
                }
                *task.cancel_flag = true; 
                });

    
            task.result = task.func(task.argument, *task.cancel_flag);
            timeout_thread.join();


            {
                lock_guard<mutex> lock(mtx);
                --active_tasks;
            }
            cv.notify_one();
            });
    }

 
    {
        unique_lock<mutex> lock(mtx);
        cv.wait(lock, [] { return active_tasks == 0; });
    }

    for (auto& t : threads) {
        t.join();
    }

    cout << "Group " << group_name << " tasks completed.\n";
}

int main() {
    // Встановити обробник сигналу Ctrl+C
    signal(SIGINT, handleCtrlC);

    cout << "Command-line interface started. Type 'help' for commands.\n";

    string input;
    while (true) {
        cout << "> ";
        getline(cin, input);
        if (input.empty()) continue;

        istringstream iss(input);
        vector<string> tokens{ istream_iterator<string>{iss}, istream_iterator<string>{} };

        if (tokens.empty()) continue;

        if (tokens[0] == "help") {
            cout << "Available commands:\n";
            cout << "  group <name>                               - Create a new group.\n";
            cout << "  switch <name>                              - Switch to a different group.\n";
            cout << "  new <name> <func> <arg> <timeout_ms>       - Add a task to the current group (square, sqrt, factorial).\n";
            cout << "  run                                        - Run all tasks in the current group.\n";
            cout << "  status                                     - Show the status of tasks in the current group.\n";
            cout << "  summary                                    - Show a summary of all groups.\n";
            cout << "  exit                                       - Exit the program.\n";
        }
        else if (tokens[0] == "group") {
            if (tokens.size() != 2) {
                cerr << "Usage: group <name>\n";
                continue;
            }
            createGroup(tokens[1]);
        }
        else if (tokens[0] == "switch") {
            if (tokens.size() != 2) {
                cerr << "Usage: switch <name>\n";
                continue;
            }
            switchGroup(tokens[1]);
        }
        else if (tokens[0] == "new") {
            if (tokens.size() != 5) {
                cerr << "Usage: new <name> <func> <arg> <timeout_ms>\n";
                continue;
            }
            if (current_group.empty()) {
                cerr << "No group selected. Use 'switch <name>' to select a group.\n";
                continue;
            }

            TaskFunction func;
            if (tokens[2] == "square") {
                func = square;
            }
            else if (tokens[2] == "sqrt") {
                func = square_root;
            }
            else if (tokens[2] == "factorial") {
                func = factorial;
            }
            else {
                cerr << "Unknown function: " << tokens[2] << "\n";
                continue;
            }

            int arg = stoi(tokens[3]);
            int timeout_ms = stoi(tokens[4]);
            addTask(current_group, tokens[1], func, arg, timeout_ms);
        }
        else if (tokens[0] == "run") {
            if (current_group.empty()) {
                cerr << "No group selected. Use 'switch <name>' to select a group.\n";
                continue;
            }
            runGroup(current_group);
        }
        else if (tokens[0] == "status") {
            if (current_group.empty()) {
                cerr << "No group selected. Use 'switch <name>' to select a group.\n";
                continue;
            }
            showStatus(current_group);
        }
        else if (tokens[0] == "summary") {
            showSummary();
        }
        else if (tokens[0] == "exit") {
            cout << "Exiting.\n";
            break;
        }
        else {
            cerr << "Unknown command. Type 'help' for a list of commands.\n";
        }
    }

    cout << "Program terminated.\n";
    return 0;
}
