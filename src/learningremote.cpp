#include "IRController.h"

#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
int parseNumber(const std::string& value, const std::string& option, int minimum, int maximum) {
  std::size_t used = 0;
  int result;
  try {
    result = std::stoi(value, &used);
  } catch (...) {
    throw std::runtime_error(option + " の値が不正です");
  }
  if (used != value.size() || result < minimum || result > maximum) {
    throw std::runtime_error(option + " の値が範囲外です");
  }
  return result;
}

void printUsage() {
  std::cout << "コマンド: learn <name> [--db FILE] [--timeout SEC], "
               "send <name> [--db FILE] [--repeat N] [--gap MS], "
               "delete <name> [--db FILE], list [--db FILE], "
               "allclear [--db FILE], quit\n";
}
}  // namespace

int main() {
  IRController controller;
  IRController::installSignalHandlers();
  std::string line;

  std::cout << "learningremote を起動しました。コマンド (learn/send/list/quit) を入力してください。\n";
  while (!controller.interrupt()) {
    std::cout << "learningremote> " << std::flush;
    if (!std::getline(std::cin, line)) {
      std::cout << "\n";
      break;
    }

    std::istringstream input(line);
    std::vector<std::string> arguments;
    std::string word;
    while (input >> word) {
      arguments.push_back(word);
    }
    if (arguments.empty()) {
      continue;
    }
    const std::string& command = arguments[0];
    if (command == "quit") {
      if (arguments.size() != 1) {
        std::cerr << "quit は引数を取りません。\n";
        continue;
      }
      break;
    }

    try {
      if (command != "learn" && command != "send" && command != "delete" &&
          command != "list" && command != "allclear") {
        printUsage();
        continue;
      }
      const bool needsName = command == "learn" || command == "send" || command == "delete";
      if (needsName && arguments.size() < 2) {
        printUsage();
        continue;
      }

      const std::string name = needsName ? arguments[1] : "";
      const std::size_t optionStart = needsName ? 2 : 1;
      std::string dbPath = "codes.db";
      int timeoutSec = 30;
      int repeat = 1;
      int gapMs = 100;
      for (std::size_t i = optionStart; i < arguments.size(); ++i) {
        const std::string& option = arguments[i];
        if (i + 1 >= arguments.size()) {
          throw std::runtime_error(option + " の値を指定してください");
        }
        const std::string& value = arguments[++i];
        if (option == "--db") {
          dbPath = value;
        } else if (command == "learn" && option == "--timeout") {
          timeoutSec = parseNumber(value, option, 1, 600);
        } else if (command == "send" && option == "--repeat") {
          repeat = parseNumber(value, option, 1, 20);
        } else if (command == "send" && option == "--gap") {
          gapMs = parseNumber(value, option, 20, 2000);
        } else {
          throw std::runtime_error("不明または使用できないオプションです: " + option);
        }
      }

      if (command == "learn") {
        controller.learn(name, dbPath, timeoutSec);
      } else if (command == "send") {
        controller.send(name, dbPath, repeat, gapMs);
      } else if (command == "delete") {
        controller.deleteSignal(name, dbPath);
      } else if (command == "allclear") {
        controller.clearAll(dbPath);
      } else {
        controller.list(dbPath);
      }
    } catch (const std::exception& error) {
      std::cerr << "エラー: " << error.what() << '\n';
    }
  }
  std::cout << "learningremote を終了します。\n";
  return 0;
}
