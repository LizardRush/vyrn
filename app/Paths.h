// Where things are: data dir (python/, templates/, docs), projects root, helper programs.
#pragma once

#include <QString>

namespace paths {

void init(const char* argv0);   // call once from main() before anything else
QString dataDir();              // vyrn::find_data_dir(argv0); "" when not found
QString projectsRoot();         // vyrn::projects_root() (~/Vyrn, created when missing)
QString templatesDir();         // dataDir() + "/templates"
QString focusHelper();          // vyrn-focus next to this executable, else on PATH, else ""
QString cliPath();              // vyrn-cli next to this executable, else on PATH, else ""
// Where this system's installer puts vyrn-cli: ~/.local/bin/vyrn-cli, ~/Applications/Vyrn.app/Contents/MacOS/vyrn-cli,
// %LOCALAPPDATA%\Programs\Vyrn\bin\vyrn-cli.exe (next to the installed app, so cliPath() finds it there first).
QString installedCli();
QString panicScript();          // ~/.local/bin/vyrn-panic, else next to this executable, else ""
QString cacheDir();             // QStandardPaths::CacheLocation (created)

QString installScript();        // <source tree>/install.sh when this build has one, else ""
// How Vyrn (with vyrn-cli, its Python library and templates) is installed on this system, to show people:
// "./install.sh --app", "./install-mac.sh" or "install-windows.ps1" (run in the folder Vyrn was downloaded into).
QString installCommand();
QString pretty(const QString& path);   // the path with the home folder written as "~"

}  // namespace paths
