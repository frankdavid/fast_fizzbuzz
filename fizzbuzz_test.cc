#include <fstream>
#include <cmath>
#include <iostream>

static void assert(bool pass, std::string& actual, int64_t number) {
  if (!pass) {
    std::cerr << "Wrong output for number " << number << ": \"" << actual << "\"\n";
    exit(0);
  }
}

static void CheckFizzBuzzLine(std::string& line, int64_t number) {
  if (number % 15 == 0) {
    assert(line == "FizzBuzz", line, number);
  } else if (number % 3 == 0) {
    assert(line == "Fizz", line, number);

  } else if (number % 5 == 0) {
    assert(line == "Buzz", line, number);
  } else {
    std::string expected = std::to_string(number);
    assert(line == expected, line, number);
  }

  if (number % 100000000 == 1) {
    std::cerr << "Good until " << number << "\n";
  }
}

int main() {
  std::string line;
  int64_t number = 1;
  while (std::getline(std::cin, line)) {
    CheckFizzBuzzLine(line, number);
    ++number;
  }
}