#include "Localization.h"
#include <QDir>
#include <QCoreApplication>
#include <QDebug>
#include <QLocale>
#include <QFileInfo>
#include <QHash>

QTranslator *Localization::m_translator = nullptr;
QString Localization::m_localeDir;
QString Localization::m_currentLang = QStringLiteral("en");
QString Localization::m_detectedSystemLang = QStringLiteral("en");

static QString normalizeCode(QString code) {
    code.replace(QChar('_'), QChar('-'));
    return code;
}

QStringList Localization::buildCandidates(const QLocale &locale) {
    QStringList candidates;
    auto add = [&candidates](const QString &c) {
        const QString norm = normalizeCode(c);
        if (!norm.isEmpty() && !candidates.contains(norm))
            candidates.append(norm);
    };

    // QLocale::uiLanguages() returns e.g. "es-ES", "es" for a Spanish locale
    const QStringList uiLangs = locale.uiLanguages();
    for (const QString &l : uiLangs) {
        add(l);
        add(QLocale(l).name()); // normalized variant (es_ES -> es-ES)
    }
    add(locale.name());

    // Language-only codes and their shipped base-language fallback
    // (es-MX -> es-ES, since only rufus_es-ES.qm is shipped)
    for (const QString &l : uiLangs) {
        const QString base = l.left(2);
        add(base);
        add(matchBaseLanguage(base));
    }

    // English as a last resort so the welcome dialog and UI stay usable
    add(QStringLiteral("en"));
    return candidates;
}

QString Localization::matchBaseLanguage(const QString &base) {
    const QStringList available = availableLocales();
    static const QHash<QString, QString> preferred = {
        {"ar", "ar-SA"}, {"bg", "bg-BG"}, {"cs", "cs-CZ"},
        {"da", "da-DK"}, {"de", "de-DE"}, {"el", "el-GR"},
        {"en", "en"},   {"es", "es-ES"}, {"fa", "fa-IR"},
        {"fi", "fi-FI"}, {"fr", "fr-FR"}, {"he", "he-IL"},
        {"hr", "hr-HR"}, {"hu", "hu-HU"}, {"id", "id-ID"},
        {"it", "it-IT"}, {"ja", "ja-JP"}, {"ko", "ko-KR"},
        {"lt", "lt-LT"}, {"lv", "lv-LV"}, {"ms", "ms-MY"},
        {"nb", "nb-NO"}, {"nl", "nl-NL"}, {"pl", "pl-PL"},
        {"pt", "pt-BR"}, {"ro", "ro-RO"}, {"ru", "ru-RU"},
        {"sk", "sk-SK"}, {"sl", "sl-SI"}, {"sr", "sr-RS"},
        {"sv", "sv-SE"}, {"th", "th-TH"}, {"tr", "tr-TR"},
        {"uk", "uk-UA"}, {"vi", "vi-VN"}, {"zh", "zh-CN"}
    };

    const QString pref = preferred.value(base.toLower());
    if (!pref.isEmpty() && available.contains(pref))
        return pref;

    // Any shipped file with the same base language
    for (const QString &code : available) {
        if (code.left(2).compare(base, Qt::CaseInsensitive) == 0)
            return code;
    }
    return QString();
}

QString Localization::tryLoadTranslation(const QString &code,
                                         const QStringList &searchPaths) {
    if (code.isEmpty())
        return QString();
    for (const QString &path : searchPaths) {
        const QString fullPath =
            path + QStringLiteral("/rufus_") + normalizeCode(code) + QStringLiteral(".qm");
        if (QFileInfo::exists(fullPath) && m_translator->load(fullPath))
            return normalizeCode(code);
    }
    return QString();
}

void Localization::init(const QString &appDir) {
    m_localeDir = appDir + QStringLiteral("/translations");
    m_translator = new QTranslator;

    const QLocale locale = QLocale::system();
    m_detectedSystemLang = locale.name();

    QStringList searchPaths = {
        m_localeDir,
        appDir + QStringLiteral("/../res/translations"),
        appDir + QStringLiteral("/../../res/translations"),
        appDir + QStringLiteral("/../bin/translations"),
        appDir + QStringLiteral("/../share/rufus/translations"),
        QStringLiteral("/usr/share/rufus/translations"),
        QStringLiteral("/usr/local/share/rufus/translations"),
        QCoreApplication::applicationDirPath() + QStringLiteral("/../Resources/translations")
    };

    // Try the candidate codes in order of preference (exact region first,
    // then language-only, then base-language fallback, then English)
    const QStringList candidates = buildCandidates(locale);
    for (const QString &code : candidates) {
        const QString loaded = tryLoadTranslation(code, searchPaths);
        if (!loaded.isEmpty()) {
            QCoreApplication::installTranslator(m_translator);
            m_currentLang = loaded;
            qDebug().noquote() << QStringLiteral("Loaded translations from:")
                               << m_translator->filePath();
            return;
        }
    }

    delete m_translator;
    m_translator = nullptr;
    m_currentLang = QStringLiteral("en");
    qDebug() << "No translations found, using English";
}

bool Localization::setLanguage(const QString &langCode) {
    QString appDir = QCoreApplication::applicationDirPath();
    QStringList searchPaths = {
        m_localeDir,
        appDir + QStringLiteral("/translations"),
        appDir + QStringLiteral("/../res/translations"),
        appDir + QStringLiteral("/../../res/translations"),
        appDir + QStringLiteral("/../bin/translations"),
        appDir + QStringLiteral("/../share/rufus/translations"),
        QStringLiteral("/usr/share/rufus/translations"),
        QStringLiteral("/usr/local/share/rufus/translations"),
    };

    // Remove old translator
    if (m_translator) {
        QCoreApplication::removeTranslator(m_translator);
        delete m_translator;
        m_translator = nullptr;
    }

    m_translator = new QTranslator;

    const QStringList candidates = buildCandidates(QLocale(langCode));
    for (const QString &code : candidates) {
        const QString loaded = tryLoadTranslation(code, searchPaths);
        if (!loaded.isEmpty()) {
            QCoreApplication::installTranslator(m_translator);
            m_currentLang = loaded;
            qDebug().noquote() << QStringLiteral("Set language to:")
                               << m_translator->filePath();
            return true;
        }
    }

    // Not found — fall back to English
    delete m_translator;
    m_translator = nullptr;
    m_currentLang = QStringLiteral("en");
    qDebug() << "Translation not found for" << langCode << ", keeping English";
    return false;
}

QString Localization::translate(const QString &context, const QString &source) {
    if (m_translator && !m_translator->isEmpty())
        return m_translator->translate(context.toUtf8().constData(),
                                        source.toUtf8().constData());
    return source;
}

// Returns the translation file codes (as in rufus_<code>.qm: "en",
// "bg-BG", "es-ES", ...) for the languages shipped with the app.
QList<QString> Localization::availableLocales() {
    QStringList codes;
    QStringList searchPaths = {
        m_localeDir,
        QCoreApplication::applicationDirPath() + QStringLiteral("/translations")
    };

    for (const QString &dirPath : searchPaths) {
        QDir dir(dirPath);
        if (!dir.exists()) continue;
        for (const QString &entry : dir.entryList({QStringLiteral("rufus_*.qm")})) {
            QString lang = entry.section(QStringLiteral("_"), 1).section(QStringLiteral("."), 0, 0);
            if (!codes.contains(lang))
                codes.append(lang);
        }
        if (!codes.isEmpty()) break;
    }

    if (codes.isEmpty())
        codes.append(QStringLiteral("en"));
    return codes;
}

QString Localization::localePath() {
    return m_localeDir;
}

QString Localization::currentLanguage() {
    return m_currentLang;
}

QString Localization::detectedSystemLanguage() {
    return m_detectedSystemLang;
}
