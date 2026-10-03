#include "reminders/ical.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>

namespace rem {

namespace {

using namespace std::chrono;

std::string upper(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

bool iequals(std::string_view a, std::string_view b) { return upper(a) == upper(b); }

// Content lines with folding undone.
std::vector<std::string> unfold(std::string_view text) {
    std::vector<std::string> lines;
    std::size_t i = 0;
    while (i < text.size()) {
        auto nl = text.find('\n', i);
        auto line = text.substr(i, nl == std::string_view::npos ? std::string_view::npos : nl - i);
        i = nl == std::string_view::npos ? text.size() : nl + 1;
        if (line.ends_with('\r')) line.remove_suffix(1);
        if (!line.empty() && (line[0] == ' ' || line[0] == '\t') && !lines.empty())
            lines.back() += line.substr(1);
        else if (!line.empty())
            lines.emplace_back(line);
    }
    return lines;
}

std::optional<IcalProperty> parse_line(std::string_view line) {
    IcalProperty p;
    std::size_t i = 0;
    while (i < line.size() && line[i] != ';' && line[i] != ':') ++i;
    p.name = upper(line.substr(0, i));
    if (p.name.empty() || i == line.size()) return std::nullopt;
    while (line[i] == ';') {
        auto start = ++i;
        while (i < line.size() && line[i] != '=' && line[i] != ';' && line[i] != ':') ++i;
        auto pname = upper(line.substr(start, i - start));
        std::string pvalue;
        if (i < line.size() && line[i] == '=') {
            ++i;
            for (bool quoted = false; i < line.size(); ++i) {
                char c = line[i];
                if (c == '"') quoted = !quoted;
                else if (!quoted && (c == ';' || c == ':')) break;
                else pvalue += c;
            }
        }
        p.params.emplace_back(std::move(pname), std::move(pvalue));
        if (i >= line.size()) return std::nullopt;
    }
    p.value = std::string(line.substr(i + 1));
    return p;
}

void write_folded(std::string& out, const std::string& line) {
    std::size_t start = 0, limit = 75;
    while (line.size() - start > limit) {
        auto end = start + limit;
        while (end > start && (static_cast<unsigned char>(line[end]) & 0xC0) == 0x80) --end;  // UTF-8 continuation
        out.append(line, start, end - start);
        out += "\r\n ";
        start = end;
        limit = 74;  // the leading space counts
    }
    out.append(line, start);
    out += "\r\n";
}

void write_component(std::string& out, const IcalComponent& c) {
    write_folded(out, "BEGIN:" + c.name);
    for (auto& p : c.props) {
        std::string line = p.name;
        for (auto& [k, v] : p.params) {
            bool quote = v.find_first_of(":;,") != std::string::npos;
            line += ";" + k + "=" + (quote ? "\"" + v + "\"" : v);
        }
        line += ":" + p.value;
        write_folded(out, line);
    }
    for (auto& ch : c.children) write_component(out, ch);
    write_folded(out, "END:" + c.name);
}

std::optional<int> number(std::string_view s) {
    int n = 0;
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), n);
    if (ec != std::errc{} || p != s.data() + s.size()) return std::nullopt;
    return n;
}

}  // namespace

std::optional<std::string> IcalProperty::param(std::string_view n) const {
    for (auto& [k, v] : params)
        if (iequals(k, n)) return v;
    return std::nullopt;
}

const IcalProperty* IcalComponent::prop(std::string_view n) const {
    for (auto& p : props)
        if (p.name == n) return &p;
    return nullptr;
}

std::vector<const IcalProperty*> IcalComponent::all(std::string_view n) const {
    std::vector<const IcalProperty*> out;
    for (auto& p : props)
        if (p.name == n) out.push_back(&p);
    return out;
}

void IcalComponent::set(std::string_view n, std::string value, std::vector<std::pair<std::string, std::string>> params) {
    IcalProperty p{std::string(n), std::move(params), std::move(value)};
    auto it = std::ranges::find_if(props, [&](auto& q) { return q.name == n; });
    if (it == props.end()) {
        props.push_back(std::move(p));
        return;
    }
    *it = std::move(p);
    auto at = it - props.begin();
    for (auto j = props.size(); j-- > static_cast<std::size_t>(at) + 1;)
        if (props[j].name == n) props.erase(props.begin() + static_cast<std::ptrdiff_t>(j));
}

void IcalComponent::remove(std::string_view n) {
    std::erase_if(props, [&](auto& p) { return p.name == n; });
}

IcalComponent* IcalComponent::child(std::string_view n) {
    for (auto& c : children)
        if (c.name == n) return &c;
    return nullptr;
}

const IcalComponent* IcalComponent::child(std::string_view n) const {
    for (auto& c : children)
        if (c.name == n) return &c;
    return nullptr;
}

std::optional<IcalComponent> parse_ical(std::string_view text) {
    std::vector<IcalComponent> stack;
    std::optional<IcalComponent> top;
    for (auto& line : unfold(text)) {
        auto p = parse_line(line);
        if (!p) continue;
        if (p->name == "BEGIN") {
            stack.push_back(IcalComponent{upper(p->value), {}, {}});
        } else if (p->name == "END") {
            if (stack.empty() || stack.back().name != upper(p->value)) return std::nullopt;
            auto done = std::move(stack.back());
            stack.pop_back();
            if (stack.empty()) {
                top = std::move(done);
                break;
            }
            stack.back().children.push_back(std::move(done));
        } else if (!stack.empty()) {
            stack.back().props.push_back(std::move(*p));
        }
    }
    if (!top || top->name != "VCALENDAR") return std::nullopt;
    return top;
}

std::string serialize_ical(const IcalComponent& c) {
    std::string out;
    write_component(out, c);
    return out;
}

std::string ical_text(std::string_view v) {
    std::string out;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (v[i] == '\\' && i + 1 < v.size()) {
            char n = v[++i];
            out += (n == 'n' || n == 'N') ? '\n' : n;
        } else {
            out += v[i];
        }
    }
    return out;
}

std::string ical_escape(std::string_view t) {
    std::string out;
    for (char c : t) {
        if (c == '\\' || c == ';' || c == ',') {
            out += '\\';
            out += c;
        } else if (c == '\n') {
            out += "\\n";
        } else if (c != '\r') {
            out += c;
        }
    }
    return out;
}

std::vector<std::string> ical_text_list(std::string_view v) {
    std::vector<std::string> out;
    std::string cur;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (v[i] == '\\' && i + 1 < v.size()) {
            cur += v[i];
            cur += v[++i];
        } else if (v[i] == ',') {
            out.push_back(ical_text(cur));
            cur.clear();
        } else {
            cur += v[i];
        }
    }
    out.push_back(ical_text(cur));
    std::erase_if(out, [](auto& s) { return s.empty(); });
    return out;
}

std::optional<IcalWhen> ical_when(const IcalProperty& p, const time_zone* local) {
    std::string_view v = p.value;
    if (v.size() < 8) return std::nullopt;
    auto y = number(v.substr(0, 4)), m = number(v.substr(4, 2)), d = number(v.substr(6, 2));
    if (!y || !m || !d) return std::nullopt;
    Date date{year{*y}, month{static_cast<unsigned>(*m)}, day{static_cast<unsigned>(*d)}};
    if (!date.ok()) return std::nullopt;
    if (v.size() == 8) return IcalWhen{date, std::nullopt};
    if (v.size() < 15 || v[8] != 'T') return std::nullopt;
    auto hh = number(v.substr(9, 2)), mm = number(v.substr(11, 2)), ss = number(v.substr(13, 2));
    if (!hh || !mm || !ss || *hh > 23 || *mm > 59) return std::nullopt;
    auto tod = hours{*hh} + minutes{*mm} + seconds{*ss};

    // A moment in time (UTC or a named zone) shown in local time.
    std::optional<sys_seconds> instant;
    if (v.size() == 16 && v[15] == 'Z') {
        instant = sys_days{date} + tod;
    } else if (auto tzid = p.param("TZID")) {
        try {
            auto zone = locate_zone(*tzid);
            instant = zone->to_sys(local_days{date} + tod, choose::earliest);
        } catch (const std::exception&) {
            // Not an IANA name (a server's own TZID): treat the time as local.
        }
    }
    if (!instant || !local) return IcalWhen{date, TimeOfDay{*hh, *mm}};
    auto lt = local->to_local(*instant);
    auto ld = floor<days>(lt);
    hh_mm_ss hms{floor<minutes>(lt - ld)};
    return IcalWhen{Date{ld}, TimeOfDay{static_cast<int>(hms.hours().count()), static_cast<int>(hms.minutes().count())}};
}

std::string ical_utc(sys_seconds t) {
    auto dd = floor<days>(t);
    Date d{dd};
    hh_mm_ss hms{t - dd};
    return std::format("{:04}{:02}{:02}T{:02}{:02}{:02}Z", static_cast<int>(d.year()), static_cast<unsigned>(d.month()),
                       static_cast<unsigned>(d.day()), hms.hours().count(), hms.minutes().count(),
                       hms.seconds().count());
}

std::string ical_date(Date d) {
    return std::format("{:04}{:02}{:02}", static_cast<int>(d.year()), static_cast<unsigned>(d.month()),
                       static_cast<unsigned>(d.day()));
}

std::string ical_local(Date d, TimeOfDay t) { return std::format("{}T{:02}{:02}00", ical_date(d), t.hour, t.minute); }

}  // namespace rem
