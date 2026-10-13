#ifndef IR_CONTROLLER_H
#define IR_CONTROLLER_H

#include <string>

class IRController {
 public:
  static void installSignalHandlers();
  bool interrupt() const;

  void learn(const std::string& name, const std::string& dbPath, int timeoutSec);
  void send(const std::string& name, const std::string& dbPath, int repeat, int gapMs);
  void deleteSignal(const std::string& name, const std::string& dbPath) const;
  void list(const std::string& dbPath) const;
  void clearAll(const std::string& dbPath) const;
};

#endif  // IR_CONTROLLER_H
