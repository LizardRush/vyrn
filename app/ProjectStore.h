// Projects for the UI: a thin Qt wrapper over core's vyrn/project.h.
#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>

struct TemplateCard {
  QString id, name, mode, description, path;   // mode: "sim" | "realtime"
  bool demo = false;                           // a demo, listed after the presets
};

struct ProjectCard {
  QString name, path, mode, templateId;
  QString trainingBy;                           // "", "app", "agent", "cli"
  bool trusted = true;                          // false: imported from a .vrlp and not trusted yet (vyrn::is_trusted)
  bool hasBest = false;
  double best = 0;                              // best 20-round mean points
  double timeTrained = 0;                       // seconds
  qint64 modified = 0;                          // seconds since the epoch
};

// A project's Vyrn Link settings: vyrn.json "link" (docs/dev/link.md). Off when it has none.
struct LinkSettings {
  bool on = false;
  int port = 7457;
  bool lockstep = false;
  int framesPerStep = 1;
};

struct BrainCard {
  QString id, updated;
  bool active = false, hasBest = false;
  double best = 0, timeTrained = 0;
  qint64 ticks = 0;
};

namespace store {

QList<TemplateCard> templates();
QList<ProjectCard> projects();                  // newest first
ProjectCard project(const QString& path);       // empty name when it is not a project
QString createProject(const QString& name, const TemplateCard& tmpl, QString* error);   // new path or ""
bool renameProject(const QString& path, const QString& newName, QString* error);       // sets vyrn.json "name"
QString duplicateProject(const QString& path, QString* error);                         // new path or ""
bool deleteProject(const QString& path, QString* error);                               // moves it to the trash
QList<BrainCard> brains(const QString& path);
// A brain with a save (training ran long enough to keep it): what Compile and Export can use.
bool isTrained(const BrainCard& brain);
// The project's active brain has a save (Compile's toolbar button is enabled).
bool hasTrainedBrain(const QString& path);
QJsonObject settings(const QString& path);
bool mergeSettings(const QString& path, const QJsonObject& changes, QString* error);
QString lockHolder(const QString& path, int* pid = nullptr);   // "", "app", "agent", "cli"

// Vyrn Link (vyrn/link.h): "link" read like the core reads it (bad values fall back to their defaults), the
// project's pairing code (made and saved when missing), a new code ("" + error when it can't be saved), and the
// address a game connects to from another computer.
LinkSettings linkSettings(const QJsonObject& settings);
QString linkCode(const QString& path);
QString newLinkCode(const QString& path, QString* error);
QString linkUrl(int port);

QString folderNameFor(const QString& name);     // "My Game!" -> "My-Game" (vyrn::folder_name_for)
QString folderFor(const QString& name);         // projectsRoot()/folderNameFor(name)
// `base` itself when its folder is free, else "base 2", "base 3", ...
QString uniqueName(const QString& base);
// Plain words for a duration: "45 s", "12 min", "2 h 5 min".
QString formatDuration(double seconds);
// Points with sensible precision: "0.85", "14.2", "1,203".
QString formatPoints(double points);

}  // namespace store
