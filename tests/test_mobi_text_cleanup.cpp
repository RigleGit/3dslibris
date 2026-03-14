#include "mobi_text_cleanup.h"

#include <cstdlib>
#include <string>

namespace {

[[noreturn]] void Fail(const std::string &message) {
  fprintf(stderr, "%s\n", message.c_str());
  std::exit(1);
}

void ExpectEqual(const char *label, const std::string &actual,
                 const std::string &expected) {
  if (actual != expected) {
    Fail(std::string(label) + ": unexpected normalized text");
  }
}

} // namespace

int main() {
  using mobi_text_cleanup::FixBrokenParagraphWraps;
  using mobi_text_cleanup::RepairCommonMojibake;

  ExpectEqual("repairs mojibake acute vowels",
              RepairCommonMojibake("coraz\xc3\x83\xc2\xb3n, religi\xc3\x83\xc2\xb3n"),
              "coraz\xc3\xb3n, religi\xc3\xb3n");
  ExpectEqual("repairs mojibake i with soft hyphen marker",
              RepairCommonMojibake("as\xc3\x83\xc2\xad como"),
              "as\xc3\xad como");

  ExpectEqual("keeps utf8 acute letter when merging wraps",
              FixBrokenParagraphWraps(
                  "criatura oprimida, es el significado real del mundo sin "
                  "coraz\xc3\xb3n, as\xc3\xad\n\n"
                  "como es el esp\xc3\xadritu de una \xc3\xa9poca privada de "
                  "esp\xc3\xadritu."),
              "criatura oprimida, es el significado real del mundo sin "
              "coraz\xc3\xb3n, as\xc3\xad como es el esp\xc3\xadritu de una "
              "\xc3\xa9poca privada de esp\xc3\xadritu.");

  ExpectEqual("merges blank-line hard wraps",
              FixBrokenParagraphWraps("encontrado solo el reflejo de si mismo\n\n"
                                      "en la fantastica realidad del\n\n"
                                      "cielo, donde buscaba un superhombre"),
              "encontrado solo el reflejo de si mismo en la fantastica realidad "
              "del cielo, donde buscaba un superhombre");

  ExpectEqual("keeps headings and following paragraph split",
              FixBrokenParagraphWraps("PREFACIO\n\n"
                                      "Este es el comienzo de un capitulo."),
              "PREFACIO\n\nEste es el comienzo de un capitulo.");

  ExpectEqual("keeps sentence-ending paragraph break",
              FixBrokenParagraphWraps("Primera idea completa.\n\n"
                                      "Segunda idea independiente."),
              "Primera idea completa.\n\nSegunda idea independiente.");

  return 0;
}
