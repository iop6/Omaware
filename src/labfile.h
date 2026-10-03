// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QString>
#include <QVariantMap>

// A lab as a TOML file people can share, edit and keep in version control. It holds the lab's plan
// (networks and VMs), never passwords or keys. Reading gives a plan for LabPlan::check(), so a file
// is reviewed and built exactly like a lab an agent proposes.
namespace LabFile {
// From a built lab's normalized plan (LabPlan::check()'s "plan").
QString write(const QVariantMap &plan);
// The plan in LabPlan's input form; on failure, an empty map and `error` says what's wrong.
QVariantMap read(const QString &text, QString &error);
}
