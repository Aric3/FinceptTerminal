#include "PortfolioReader.h"
#include "Runtime.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>

#include <cstdio>

using namespace fincept::cli;
namespace {
int integer(const QCommandLineParser& parser, const QString& flag, int fallback, int min, int max) {
    if (!parser.isSet(flag))
        return fallback;
    bool ok;
    int value = parser.value(flag).toInt(&ok);
    if (!ok || value < min || value > max)
        throw Error("invalid_argument", flag + " is outside its allowed range");
    return value;
}
QString symbol(QString value) {
    value = value.trimmed().toUpper();
    static const QRegularExpression valid("^[A-Z0-9^][A-Z0-9.^=_/-]{0,63}$");
    if (!valid.match(value).hasMatch())
        throw Error("invalid_symbol", "Use one Yahoo symbol, for example AAPL, ^GSPC or 0700.HK");
    return value;
}
void flags(const QCommandLineParser& parser, const QSet<QString>& allowed) {
    const QSet<QString> globals{"json", "profile", "data-root", "db", "python", "scripts", "timeout"};
    for (const auto& option : parser.optionNames())
        if (!globals.contains(option) && !allowed.contains(option))
            throw Error("invalid_argument", "Option is not supported by this command");
}
QJsonValue run(const QCommandLineParser& parser, const Runtime& runtime) {
    const auto args = parser.positionalArguments();
    if (args == QStringList{"doctor"}) {
        flags(parser, {"online"});
        return runtime.doctor(parser.isSet("online"));
    }
    if (args == QStringList{"profiles", "list"}) {
        flags(parser, {});
        return QJsonObject{{"items", profiles(runtime.root)}, {"default_profile", "default"}};
    }
    if (args.size() == 3 && args[0] == "symbols" && args[1] == "search") {
        flags(parser, {"limit"});
        if (args[2].trimmed().isEmpty() || args[2].size() > 120)
            throw Error("invalid_argument", "Search query must contain 1 to 120 characters");
        const int limit = integer(parser, "limit", 10, 1, 50);
        auto result = runtime.read_yahoo({"search", args[2], QString::number(limit)}).toObject();
        if (!result.value("results").isArray())
            throw Error("provider_invalid_json", "Search response must contain a results array", 4);
        const auto candidates = result.value("results").toArray();
        QJsonArray selected;
        for (int i = 0; i < qMin(limit, static_cast<int>(candidates.size())); ++i)
            selected.append(candidates[i]);
        result.insert("results", selected);
        result.insert("count", selected.size());
        return result;
    }
    if (args.size() == 3 && args[0] == "symbols" && args[1] == "resolve") {
        flags(parser, {});
        return runtime.read_yahoo({"resolve_symbol", symbol(args[2])});
    }
    if (args.size() == 3 && args[0] == "quotes" && args[1] == "get") {
        flags(parser, {});
        const auto quote = runtime.read_yahoo({"quote", symbol(args[2])}).toObject();
        if (quote.value("price").toDouble() <= 0)
            throw Error("quote_unavailable", "Provider returned no valid price", 4);
        return quote;
    }
    if (args.size() == 3 && args[0] == "history" && args[1] == "get") {
        flags(parser, {"period", "interval", "limit"});
        const QString period = parser.value("period"), interval = parser.value("interval");
        if (!QSet<QString>{"1d", "5d", "1mo", "3mo", "6mo", "1y", "2y", "5y", "10y", "ytd", "max"}.contains(period) ||
            !QSet<QString>{"1m", "2m", "5m", "15m", "30m", "60m", "90m", "1h", "1d", "5d", "1wk", "1mo", "3mo"}
                 .contains(interval))
            throw Error("invalid_argument", "Unsupported Yahoo period or interval");
        const int limit = integer(parser, "limit", 500, 1, 10000);
        const auto response = runtime.read_yahoo({"historical_period", symbol(args[2]), period, interval});
        if (!response.isArray())
            throw Error("provider_invalid_json", "Historical response must be an array", 4);
        const auto rows = response.toArray();
        QJsonArray selected;
        for (int i = qMax(0, static_cast<int>(rows.size()) - limit); i < rows.size(); ++i)
            selected.append(rows[i]);
        return QJsonObject{{"symbol", symbol(args[2])},
                           {"period", period},
                           {"interval", interval},
                           {"bars", selected},
                           {"returned", selected.size()},
                           {"available", rows.size()},
                           {"truncated", rows.size() > limit},
                           {"order", "oldest_first"}};
    }
    if (args.size() == 4 && args[0] == "data" && args[1] == "read") {
        flags(parser, {});
        if (!QSet<QString>{"quote", "info", "resolve_symbol"}.contains(args[2]))
            throw Error("invalid_argument", "Read action must be quote, info or resolve_symbol");
        return runtime.read_yahoo({args[2], symbol(args[3])});
    }
    if (args.size() >= 2 && args[0] == "portfolio") {
        const auto action = args[1];
        if (action == "list" && args.size() == 2) {
            flags(parser, {"limit", "offset"});
            const int limit = integer(parser, "limit", 100, 1, 1000);
            const int offset = integer(parser, "offset", 0, 0, 1000000);
            return PortfolioReader(runtime.db_path).list(limit, offset);
        }
        if (action == "resolve" && args.size() == 2 && parser.isSet("name")) {
            flags(parser, {"name"});
            return QJsonObject{{"id", PortfolioReader(runtime.db_path).resolve(parser.value("name"))}};
        }
        if (action == "get" && args.size() == 3) {
            flags(parser, {});
            return PortfolioReader(runtime.db_path).get(args[2]);
        }
        if (action == "value" && args.size() == 3) {
            flags(parser, {"prices", "live"});
            if (parser.isSet("prices") == parser.isSet("live"))
                throw Error("invalid_argument", "Choose exactly one of --prices FILE or --live");
            return PortfolioReader(runtime.db_path)
                .value(args[2], parser.value("prices"), parser.isSet("live"), runtime);
        }
    }
    throw Error("invalid_command", "Unknown command or wrong number of arguments; use --help");
}
void output(const QJsonObject& object, bool compact) {
    const auto bytes = QJsonDocument(object).toJson(compact ? QJsonDocument::Compact : QJsonDocument::Indented);
    fwrite(bytes.constData(), 1, bytes.size(), stdout);
    if (compact)
        fputc('\n', stdout);
}
} // namespace
int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName("fincept-local");
    QCoreApplication::setApplicationVersion("1.0.0");
    QCommandLineParser parser;
    parser.setApplicationDescription(
        "Read-only Fincept Local CLI. Does not start the desktop app, migrate databases or submit orders.\n\n"
        "Commands:\n"
        "  doctor [--online]                   Check setup; optional Yahoo reachability\n"
        "  profiles list                       Discover existing profile names\n"
        "  symbols search QUERY [--limit N]     Find Yahoo ticker candidates\n"
        "  symbols resolve SYMBOL              Verify/resolve Yahoo ticker (may try .NS/.BO)\n"
        "  quotes get SYMBOL                   Latest available daily price, not a streaming quote\n"
        "  history get SYMBOL                  Period history; --period/--interval/--limit\n"
        "  portfolio list                      Discover portfolio IDs; --limit/--offset\n"
        "  portfolio resolve --name NAME       Resolve exact name; duplicates fail\n"
        "  portfolio get ID                    Read stored holdings\n"
        "  portfolio value ID --prices FILE    Offline valuation with explicit price marks\n"
        "  portfolio value ID --live           Yahoo valuation; no FX conversion or broker credentials\n"
        "  data read ACTION SYMBOL             Raw read: quote, info, resolve_symbol\n\n"
        "All commands emit a versioned JSON envelope. No write commands. Default profile: default.");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument("command", "Command and its arguments", "command [arguments...]");
    parser.addOptions({{"json", "Compact JSON (default is pretty JSON)"},
                       {"data-root", "Existing local application data root", "path", default_root()},
                       {"profile", "Explicit profile name (does not change app selection)", "name", "default"},
                       {"db", "Read an explicit existing SQLite database instead of profile DB", "path"},
                       {"python", "Existing Python executable with yfinance/pandas", "path"},
                       {"scripts", "Directory containing existing yfinance_data.py", "path", FINCEPT_CLI_SCRIPTS},
                       {"timeout", "Total command budget in seconds (1..600)", "seconds", "120"},
                       {"online", "Perform a public Yahoo ^GSPC read during doctor"},
                       {"period", "Yahoo period", "period", "1mo"},
                       {"interval", "Yahoo interval", "interval", "1d"},
                       {"limit", "Maximum results (history: latest N bars)", "number"},
                       {"offset", "Portfolio list offset", "number"},
                       {"name", "Exact portfolio name", "name"},
                       {"prices", "JSON price file with currency and prices object", "path"},
                       {"live", "Fetch current Yahoo quotes for stored holdings"}});
    const bool compact = application.arguments().contains("--json");
    try {
        if (!parser.parse(application.arguments()))
            throw Error("invalid_argument", "Unknown option or missing option value; use --help");
        if (parser.isSet("help"))
            parser.showHelp();
        if (parser.isSet("version"))
            parser.showVersion();
        const auto proxy = fincept::network::ProxySettings::load();
        if (!proxy.validation_error().isEmpty())
            throw Error("invalid_proxy", "Saved application proxy is invalid; correct it in app settings", 3);
        const auto root = QFileInfo(parser.value("data-root")).absoluteFilePath();
        const auto profile = parser.value("profile");
        const auto database = profile_database(root, profile);
        const auto python = parser.isSet("python") ? parser.value("python") : root + "/venv-numpy1/bin/python3";
        Runtime runtime{root,
                        profile,
                        parser.isSet("db") ? parser.value("db") : database,
                        python,
                        parser.value("scripts"),
                        proxy,
                        QDeadlineTimer(integer(parser, "timeout", 120, 1, 600) * 1000)};
        output({{"schema_version", 1}, {"ok", true}, {"data", run(parser, runtime)}}, compact);
        return 0;
    } catch (const Error& error) {
        output({{"schema_version", 1},
                {"ok", false},
                {"error", QJsonObject{{"code", error.code}, {"message", QString::fromUtf8(error.what())}}}},
               compact);
        return error.status;
    } catch (const std::exception&) {
        output({{"schema_version", 1},
                {"ok", false},
                {"error", QJsonObject{{"code", "internal_error"}, {"message", "Unexpected CLI error"}}}},
               compact);
        return 1;
    }
}
