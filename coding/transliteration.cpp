#include "coding/transliteration.hpp"

#include "coding/string_utf8_multilang.hpp"

#include "base/logging.hpp"
#include "base/string_utils.hpp"

#include <unicode/uclean.h>
#include <unicode/unistr.h>
#include <unicode/utypes.h>
#include <unicode/translit.h>
#include <unicode/utrans.h>

#include <cstring>
#include <mutex>

struct Transliteration::TransliteratorInfo
{
  TransliteratorInfo()
    : m_initialized(false)
  {}

  std::atomic<bool> m_initialized;
  std::mutex m_mutex;
  std::unique_ptr<Transliterator> m_transliterator;
};

Transliteration::Transliteration()
  : m_mode(Mode::Enabled)
{}

Transliteration::~Transliteration()
{
  // The use of u_cleanup() just before an application terminates is optional,
  // but it should be called only once for performance reasons.
  // The primary benefit is to eliminate reports of memory or resource leaks originating
  // in ICU code from the results generated by heap analysis tools.
  m_transliterators.clear();
  u_cleanup();
}

Transliteration & Transliteration::Instance()
{
  static Transliteration instance;
  return instance;
}

void Transliteration::Init(std::string const & icuDataDir)
{
  // This function should be called at most once in a process,
  // before the first ICU operation that will require the loading of an ICU data file.
  // This function is not thread-safe. Use it before calling ICU APIs from multiple threads.
  u_setDataDirectory(icuDataDir.c_str());

  for (auto const & lang : StringUtf8Multilang::GetSupportedLanguages())
  {
    if (strlen(lang.m_transliteratorId) == 0 || m_transliterators.count(lang.m_transliteratorId) != 0)
      continue;

    m_transliterators.emplace(lang.m_transliteratorId, std::make_unique<TransliteratorInfo>());
  }
}

void Transliteration::SetMode(Transliteration::Mode mode)
{
  m_mode = mode;
}

bool Transliteration::Transliterate(std::string const & str, int8_t langCode, std::string & out) const
{
  if (m_mode != Mode::Enabled)
    return false;

  if (str.empty() || strings::IsASCIIString(str))
    return false;

  std::string transliteratorId(StringUtf8Multilang::GetTransliteratorIdByCode(langCode));

  if (transliteratorId.empty())
    return false;

  auto it = m_transliterators.find(transliteratorId);
  if (it == m_transliterators.end())
  {
    LOG(LWARNING, ("Transliteration failed, unknown transliterator \"", transliteratorId, "\""));
    return false;
  }

  if (!it->second->m_initialized)
  {
    std::lock_guard<std::mutex> lock(it->second->m_mutex);
    if (!it->second->m_initialized)
    {
      UErrorCode status = U_ZERO_ERROR;

      std::string const removeDiacriticRule = ";NFD;[\u02B9-\u02D3\u0301-\u0358\u00B7\u0027]Remove;NFC";
      transliteratorId.append(removeDiacriticRule);

      UnicodeString translitId(transliteratorId.c_str());

      it->second->m_transliterator.reset(Transliterator::createInstance(translitId, UTRANS_FORWARD, status));

      if (it->second->m_transliterator == nullptr)
        LOG(LWARNING, ("Cannot create transliterator \"", transliteratorId, "\", icu error =", status));

      it->second->m_initialized = true;
    }
  }

  if (it->second->m_transliterator == nullptr)
    return false;

  UnicodeString ustr(str.c_str());
  it->second->m_transliterator->transliterate(ustr);

  if (ustr.isEmpty())
    return false;

  ustr.toUTF8String(out);
  return true;
}
