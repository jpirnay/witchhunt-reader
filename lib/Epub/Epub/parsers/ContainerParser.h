#pragma once
#include <Print.h>
#include <SaxParser/SaxParser.h>

#include <string>

class ContainerParser final : public Print {
  enum ParserState {
    START,
    IN_CONTAINER,
    IN_ROOTFILES,
  };

  size_t remainingSize;
  SaxParser saxParser_;
  ParserState state = START;

  static void startElement(void* userData, const char* name, const char** atts);
  static void endElement(void* userData, const char* name);

 public:
  std::string fullPath;

  explicit ContainerParser(const size_t xmlSize) : remainingSize(xmlSize) {}
  ~ContainerParser() override;

  bool setup();

  size_t write(uint8_t) override;
  size_t write(const uint8_t* buffer, size_t size) override;
};
