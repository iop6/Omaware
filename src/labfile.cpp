// SPDX-License-Identifier: GPL-3.0-or-later
#include "labfile.h"
#include <QStringList>
#include <QVariantList>
#include <sstream>
#include <toml++/toml.hpp>

namespace {
toml::array strings(const QVariant &list) {
    toml::array out;
    for (const auto &v : list.toList())
        out.push_back(v.toString().toStdString());
    return out;
}

QVariant toVariant(const toml::node &node) {
    if (auto s = node.as_string()) return QString::fromStdString(s->get());
    if (auto i = node.as_integer()) return qlonglong(i->get());
    if (auto f = node.as_floating_point()) return f->get();
    if (auto b = node.as_boolean()) return b->get();
    if (auto a = node.as_array()) {
        QVariantList list;
        for (const auto &item : *a)
            list << toVariant(item);
        return list;
    }
    if (auto t = node.as_table()) {
        QVariantMap map;
        for (const auto &[key, value] : *t)
            map[QString::fromStdString(std::string(key.str()))] = toVariant(value);
        return map;
    }
    return {};
}
}

QString LabFile::write(const QVariantMap &plan) {
    toml::table root;
    root.insert("name", plan["name"].toString().toStdString());
    if (!plan["user"].toString().isEmpty()) root.insert("user", plan["user"].toString().toStdString());
    toml::array networks;
    for (const auto &v : plan["networks"].toList()) {
        const auto n = v.toMap();
        toml::table t{{"name", n["name"].toString().toStdString()}, {"type", n["type"].toString().toStdString()}};
        if (!n["subnet"].toString().isEmpty()) t.insert("subnet", n["subnet"].toString().toStdString());
        networks.push_back(std::move(t));
    }
    toml::array vms;
    for (const auto &v : plan["vms"].toList()) {
        const auto m = v.toMap();
        toml::table t{{"name", m["name"].toString().toStdString()}, {"os", m["os"].toString().toStdString()},
                {"cpus", m["cpus"].toLongLong()}, {"memory_mib", m["memoryMiB"].toLongLong()},
                {"disk_gib", m["diskGiB"].toLongLong()}};
        toml::array links;
        for (const auto &l : m["nics"].toList()) {
            const auto link = l.toMap();
            if (link["ip"].toString().isEmpty())
                links.push_back(link["network"].toString().toStdString());
            else
                links.push_back(toml::table{{"network", link["network"].toString().toStdString()},
                        {"ip", link["ip"].toString().toStdString()}});
        }
        t.insert("networks", std::move(links));
        if (!m["packages"].toList().isEmpty()) t.insert("packages", strings(m["packages"]));
        if (!m["setup"].toList().isEmpty()) t.insert("setup", strings(m["setup"]));
        vms.push_back(std::move(t));
    }
    root.insert("networks", std::move(networks));
    root.insert("vms", std::move(vms));
    std::ostringstream out;
    out << root;
    return "# OmaWare lab file. Import it from the network map (right-click the background).\n"
           "# Network types: internet, private (this computer only) or isolated (VMs only).\n"
           "# Passwords are never stored here; you set one when you build the lab.\n\n" +
           QString::fromStdString(out.str()) + "\n";
}

QVariantMap LabFile::read(const QString &text, QString &error) {
    if (text.size() > 256 * 1024) {
        error = "This lab file is too big.";
        return {};
    }
    try {
        const auto bytes = text.toUtf8();
        const auto table = toml::parse(std::string_view(bytes.constData(), size_t(bytes.size())));
        auto plan = toVariant(table).toMap();
        if (plan["name"].toString().trimmed().isEmpty()) {
            error = "The lab file needs a name.";
            return {};
        }
        if (plan["vms"].toList().isEmpty()) {
            error = "The lab file has no VMs.";
            return {};
        }
        return plan;
    } catch (const toml::parse_error &e) {
        error = QString("This isn't a valid lab file (line %1): %2")
                        .arg(e.source().begin.line)
                        .arg(QString::fromUtf8(e.description().data(), qsizetype(e.description().size())));
        return {};
    }
}
