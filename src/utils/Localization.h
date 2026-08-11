#pragma once

#include <QString>
#include <QLocale>
#include <QTranslator>
#include <QStringList>

class Localization {
public:
    static void init(const QString &appDir);
    static bool setLanguage(const QString &langCode);
    static QString translate(const QString &context, const QString &source);
    static QStringList availableLocales();
    static QString localePath();
    static QString currentLanguage();
    static QString detectedSystemLanguage();

private:
    static QStringList buildCandidates(const QLocale &locale);
    static QString matchBaseLanguage(const QString &base);
    static QString tryLoadTranslation(const QString &code, const QStringList &searchPaths);

    static QTranslator *m_translator;
    static QString m_localeDir;
    static QString m_currentLang;
    static QString m_detectedSystemLang;
};
