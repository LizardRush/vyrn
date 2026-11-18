#include "ProjectStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLocale>

#include <cmath>
#include <exception>
#include <string>

#include "Paths.h"
#include "vyrn/link.h"
#include "vyrn/package.h"
#include "vyrn/project.h"

namespace store {

namespace {

std::string utf8(const QString& text) { return text.toStdString(); }
QString qs(const std::string& text) { return QString::fromStdString(text); }

void setError(QString* error, const QString& text) {
  if (error) *error = text;
}

TemplateCard toCard(const vyrn::TemplateInfo& t) {
  return TemplateCard{qs(t.id), qs(t.name), qs(t.mode), qs(t.description), qs(t.path), t.demo};
}

vyrn::TemplateInfo toInfo(const TemplateCard& t) {
  vyrn::TemplateInfo info;
  info.id = utf8(t.id);
  info.name = utf8(t.name);
  info.mode = utf8(t.mode);
  info.description = utf8(t.description);
  info.path = utf8(t.path);
  info.demo = t.demo;
  return info;
}

ProjectCard toCard(const vyrn::ProjectInfo& p) {
  ProjectCard card;
  card.name = qs(p.name);
  card.path = qs(p.path);
  card.mode = qs(p.mode);
  card.templateId = qs(p.template_id);
  card.trainingBy = qs(p.training_by);
  try {
    card.trusted = vyrn::is_trusted(p.path);
  } catch (const std::exception&) {
    card.trusted = false;   // a marker Vyrn can't read counts as untrusted
  }
  card.hasBest = p.has_best;
  card.best = p.best_mean20;
  card.timeTrained = p.time_trained_s;
  card.modified = p.modified;
  return card;
}

BrainCard toCard(const vyrn::BrainInfo& b) {
  BrainCard card;
  card.id = qs(b.id);
  card.updated = qs(b.updated);
  card.active = b.active;
  card.hasBest = b.has_best;
  card.best = b.best_mean20;
  card.timeTrained = b.time_trained_s;
  card.ticks = b.ticks;
  return card;
}

}  // namespace

QList<TemplateCard> templates() {
  QList<TemplateCard> out;
  const QString data = paths::dataDir();
  if (data.isEmpty()) return out;
  try {
    for (const vyrn::TemplateInfo& t : vyrn::list_templates(utf8(data))) out << toCard(t);
  } catch (const std::exception&) {
    // An unreadable templates folder shows as "no templates"; the dialog explains it.
  }
  return out;
}

QList<ProjectCard> projects() {
  QList<ProjectCard> out;
  try {
    for (const vyrn::ProjectInfo& p : vyrn::list_projects(utf8(paths::projectsRoot()))) out << toCard(p);
  } catch (const std::exception&) {
    // The projects folder can't be read: the launcher shows the empty state.
  }
  return out;
}

ProjectCard project(const QString& path) {
  try {
    return toCard(vyrn::read_project(utf8(path)));
  } catch (const std::exception&) {
    return {};
  }
}

QString createProject(const QString& name, const TemplateCard& tmpl, QString* error) {
  const QString trimmed = name.simplified();
  if (trimmed.isEmpty()) {
    setError(error, QStringLiteral("Give the project a name."));
    return {};
  }
  try {
    return qs(vyrn::create_project(utf8(paths::projectsRoot()), utf8(trimmed), toInfo(tmpl)));
  } catch (const std::exception& e) {
    setError(error, qs(e.what()));
    return {};
  }
}

bool renameProject(const QString& path, const QString& newName, QString* error) {
  const QString trimmed = newName.simplified();
  if (trimmed.isEmpty()) {
    setError(error, QStringLiteral("A project needs a name."));
    return false;
  }
  return mergeSettings(path, QJsonObject{{QStringLiteral("name"), trimmed}}, error);
}

QString duplicateProject(const QString& path, QString* error) {
  try {
    return qs(vyrn::duplicate_project(utf8(path)));
  } catch (const std::exception& e) {
    setError(error, qs(e.what()));
    return {};
  }
}

bool deleteProject(const QString& path, QString* error) {
  if (project(path).name.isEmpty()) {
    setError(error, QStringLiteral("That folder isn't a Vyrn project, so Vyrn left it alone."));
    return false;
  }
  const QString by = lockHolder(path);
  if (by == QLatin1String("agent")) {
    setError(error, QStringLiteral("An AI agent is training this project right now. Stop it first."));
    return false;
  }
  if (!by.isEmpty()) {
    setError(error, QStringLiteral("This project is being trained right now. Stop it first."));
    return false;
  }
  if (!QFile::moveToTrash(path)) {
    setError(error, QStringLiteral("Vyrn couldn't move the folder to the trash. You can delete it yourself: %1")
                        .arg(paths::pretty(path)));
    return false;
  }
  return true;
}

QList<BrainCard> brains(const QString& path) {
  QList<BrainCard> out;
  try {
    for (const vyrn::BrainInfo& b : vyrn::list_brains(utf8(path))) out << toCard(b);
  } catch (const std::exception&) {
  }
  return out;
}

bool isTrained(const BrainCard& brain) { return brain.ticks > 0 || brain.hasBest || !brain.updated.isEmpty(); }

bool hasTrainedBrain(const QString& path) {
  for (const BrainCard& b : brains(path))
    if (b.active && isTrained(b)) return true;
  return false;
}

QJsonObject settings(const QString& path) {
  try {
    const std::string text = vyrn::read_settings_text(utf8(path));
    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(text));
    return doc.isObject() ? doc.object() : QJsonObject();
  } catch (const std::exception&) {
    return {};
  }
}

LinkSettings linkSettings(const QJsonObject& settings) {
  LinkSettings link;
  const QJsonValue v = settings.value(QLatin1String("link"));
  if (v.isUndefined() || v.isNull() || (v.isBool() && !v.toBool())) return link;
  link.on = true;
  if (!v.isObject()) return link;
  const QJsonObject o = v.toObject();
  auto whole = [&](const char* key, int lo, int hi, int fallback) {
    const QJsonValue n = o.value(QLatin1String(key));
    const double d = n.toDouble(std::nan(""));
    return n.isDouble() && std::isfinite(d) && d == std::floor(d) && d >= lo && d <= hi ? int(d) : fallback;
  };
  link.port = whole("port", 1024, 65535, link.port);
  link.lockstep = o.value(QLatin1String("lockstep")).toBool(false);
  link.framesPerStep = whole("frames_per_step", 1, 1000, link.framesPerStep);
  return link;
}

QString linkCode(const QString& path) { return QString::fromStdString(vyrn::link_code(utf8(path))); }

QString newLinkCode(const QString& path, QString* error) {
  std::string why;
  const std::string code = vyrn::new_link_code(utf8(path), &why);
  if (code.empty() && error) *error = QString::fromStdString(why);
  return QString::fromStdString(code);
}

QString linkUrl(int port) { return QString::fromStdString(vyrn::link_url(port)); }

bool mergeSettings(const QString& path, const QJsonObject& changes, QString* error) {
  const QByteArray text = QJsonDocument(changes).toJson(QJsonDocument::Compact);
  try {
    std::string why;
    if (vyrn::merge_settings(utf8(path), text.toStdString(), &why)) return true;
    setError(error, why.empty() ? QStringLiteral("Vyrn couldn't save vyrn.json.") : qs(why));
  } catch (const std::exception& e) {
    setError(error, qs(e.what()));
  }
  return false;
}

QString lockHolder(const QString& path, int* pid) {
  try {
    const auto holder = vyrn::ProjectLock::holder(utf8(path));
    if (!holder) return {};
    if (pid) *pid = holder->pid;
    return holder->by.empty() ? QStringLiteral("app") : qs(holder->by);
  } catch (const std::exception&) {
    return {};
  }
}

QString folderNameFor(const QString& name) { return qs(vyrn::folder_name_for(utf8(name.simplified()))); }

QString folderFor(const QString& name) { return QDir(paths::projectsRoot()).filePath(folderNameFor(name)); }

QString uniqueName(const QString& base) {
  const QString clean = base.simplified();
  if (!QFileInfo::exists(folderFor(clean))) return clean;
  for (int n = 2; n < 1000; ++n) {
    const QString candidate = QStringLiteral("%1 %2").arg(clean).arg(n);
    if (!QFileInfo::exists(folderFor(candidate))) return candidate;
  }
  return clean;
}

QString formatDuration(double seconds) {
  if (!(seconds > 0)) return QStringLiteral("0 s");
  if (seconds < 60) return QStringLiteral("%1 s").arg(qMax(1, qRound(seconds)));
  const qint64 minutes = qRound64(seconds / 60.0);
  if (minutes < 60) return QStringLiteral("%1 min").arg(minutes);
  const qint64 hours = minutes / 60;
  const qint64 rest = minutes % 60;
  return rest ? QStringLiteral("%1 h %2 min").arg(hours).arg(rest) : QStringLiteral("%1 h").arg(hours);
}

QString formatPoints(double points) {
  if (!std::isfinite(points)) return QStringLiteral("–");
  const double magnitude = std::fabs(points);
  if (magnitude >= 1000) return QLocale().toString(points, 'f', 0);
  QString text = QString::number(points, 'f', magnitude >= 100 ? 0 : magnitude >= 10 ? 1 : 2);
  if (text.contains(QLatin1Char('.'))) {
    while (text.endsWith(QLatin1Char('0'))) text.chop(1);
    if (text.endsWith(QLatin1Char('.'))) text.chop(1);
  }
  if (text == QLatin1String("-0")) text = QStringLiteral("0");
  return text;
}

}  // namespace store
