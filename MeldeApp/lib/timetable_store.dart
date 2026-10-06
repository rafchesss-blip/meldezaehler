import 'dart:convert';

import 'package:shared_preferences/shared_preferences.dart';

// Tages-Konvention wie auf der Uhr: 0=So, 1=Mo, 2=Di, 3=Mi, 4=Do, 5=Fr, 6=Sa
const List<String> dayNames = ['So', 'Mo', 'Di', 'Mi', 'Do', 'Fr', 'Sa'];

String _hm(int h, int m) => '${h.toString().padLeft(2, '0')}:${m.toString().padLeft(2, '0')}';

// Eine Stunde im Zeitraster (1. Stunde, 2. Stunde, ...): nur die Uhrzeiten,
// gilt für alle Tage.
class Slot {
  int sh, sm, eh, em;

  Slot({required this.sh, required this.sm, required this.eh, required this.em});

  Map<String, dynamic> toJson() => {'sh': sh, 'sm': sm, 'eh': eh, 'em': em};

  factory Slot.fromJson(Map<String, dynamic> j) => Slot(
    sh: (j['sh'] ?? 0) as int,
    sm: (j['sm'] ?? 0) as int,
    eh: (j['eh'] ?? 0) as int,
    em: (j['em'] ?? 0) as int,
  );

  int get startMin => sh * 60 + sm;
  int get endMin => eh * 60 + em;
  String get startLabel => _hm(sh, sm);
  String get endLabel => _hm(eh, em);
}

class Period {
  String name;
  int sh, sm, eh, em; // Start/Ende (Stunde, Minute)
  int? slot; // Index im Zeitraster; null = freie Zeit außerhalb des Rasters

  Period({
    required this.name,
    required this.sh,
    required this.sm,
    required this.eh,
    required this.em,
    this.slot,
  });

  Map<String, dynamic> toJson() => {
    'name': name,
    'sh': sh,
    'sm': sm,
    'eh': eh,
    'em': em,
    if (slot != null) 'slot': slot,
  };

  factory Period.fromJson(Map<String, dynamic> j) => Period(
    name: (j['name'] ?? '').toString(),
    sh: (j['sh'] ?? 0) as int,
    sm: (j['sm'] ?? 0) as int,
    eh: (j['eh'] ?? 0) as int,
    em: (j['em'] ?? 0) as int,
    slot: j['slot'] as int?,
  );

  String get startLabel => _hm(sh, sm);
  String get endLabel => _hm(eh, em);
}

// Anzahl der Fachfarben (Palette in subject_colors.dart)
const int subjectPaletteSize = 14;

class TimetableStore {
  final Map<int, List<Period>> days = {};
  final List<Slot> slots = [];
  // Fach (klein geschrieben) -> Farbindex; bleibt fest, auch wenn Fächer dazukommen
  final Map<String, int> subjectColors = {};
  // Fach (klein geschrieben) -> Meldeziel pro Stunde (fehlt/0 = kein Ziel)
  final Map<String, int> subjectGoals = {};

  static String subjectKey(String name) => name.trim().toLowerCase();

  List<Period> periodsFor(int day) => days.putIfAbsent(day, () => []);

  Future<void> load() async {
    final prefs = await SharedPreferences.getInstance();
    final s = prefs.getString('timetable');
    days.clear();
    if (s != null && s.isNotEmpty) {
      try {
        final map = jsonDecode(s) as Map<String, dynamic>;
        map.forEach((k, v) {
          days[int.parse(k)] = (v as List)
              .map((e) => Period.fromJson(e as Map<String, dynamic>))
              .toList();
        });
      } catch (_) {}
    }
    slots.clear();
    final r = prefs.getString('slots');
    if (r != null && r.isNotEmpty) {
      try {
        slots.addAll((jsonDecode(r) as List).map((e) => Slot.fromJson(e as Map<String, dynamic>)));
      } catch (_) {}
    }
    subjectColors.clear();
    final c = prefs.getString('subjectColors');
    if (c != null && c.isNotEmpty) {
      try {
        (jsonDecode(c) as Map<String, dynamic>).forEach((k, v) => subjectColors[k] = v as int);
      } catch (_) {}
    }
    subjectGoals.clear();
    final g = prefs.getString('subjectGoals');
    if (g != null && g.isNotEmpty) {
      try {
        (jsonDecode(g) as Map<String, dynamic>).forEach((k, v) => subjectGoals[k] = v as int);
      } catch (_) {}
    }
    linkPeriodsToSlots();
    assignColors();
  }

  Future<void> save() async {
    final prefs = await SharedPreferences.getInstance();
    final map = <String, dynamic>{};
    days.forEach((k, v) {
      map['$k'] = v.map((e) => e.toJson()).toList();
    });
    await prefs.setString('timetable', jsonEncode(map));
    await prefs.setString('slots', jsonEncode(slots.map((e) => e.toJson()).toList()));
    await prefs.setString('subjectColors', jsonEncode(subjectColors));
    await prefs.setString('subjectGoals', jsonEncode(subjectGoals));
  }

  // --- Fächer verwalten -----------------------------------------------------

  int goalOf(String name) => subjectGoals[subjectKey(name)] ?? 0;

  void setGoal(String name, int goal) {
    final k = subjectKey(name);
    if (goal <= 0) {
      subjectGoals.remove(k);
    } else {
      subjectGoals[k] = goal.clamp(1, 99);
    }
  }

  void setColorIndex(String name, int idx) =>
      subjectColors[subjectKey(name)] = idx % subjectPaletteSize;

  // Stunden pro Woche mit diesem Fach
  int lessonsPerWeek(String name) {
    final k = subjectKey(name);
    int n = 0;
    for (final list in days.values) {
      n += list.where((p) => subjectKey(p.name) == k).length;
    }
    return n;
  }

  // Fach an allen Tagen umbenennen. Gibt es den neuen Namen schon, werden die
  // Fächer zusammengelegt (Farbe und Ziel des vorhandenen Fachs bleiben).
  void renameSubject(String oldName, String newName) {
    final oldKey = subjectKey(oldName);
    final newKey = subjectKey(newName);
    final name = newName.trim();
    if (name.isEmpty) return;
    final merge = oldKey != newKey && knownSubjects().any((s) => subjectKey(s) == newKey);
    for (final list in days.values) {
      for (final p in list) {
        if (subjectKey(p.name) == oldKey) p.name = name;
      }
    }
    if (oldKey == newKey) return;
    final color = subjectColors.remove(oldKey);
    final goal = subjectGoals.remove(oldKey);
    if (!merge) {
      if (color != null) subjectColors[newKey] = color;
      if (goal != null) subjectGoals[newKey] = goal;
    } else if (goal != null && !subjectGoals.containsKey(newKey)) {
      subjectGoals[newKey] = goal;
    }
  }

  // Fach an allen Tagen entfernen
  void deleteSubject(String name) {
    final k = subjectKey(name);
    for (final list in days.values) {
      list.removeWhere((p) => subjectKey(p.name) == k);
    }
    subjectColors.remove(k);
    subjectGoals.remove(k);
  }

  // --- Fachfarben -----------------------------------------------------------

  // Jedem Fach ohne Farbe die erste Farbe geben, die kein anderes eingetragenes
  // Fach hat (erst wenn alle vergeben sind, doppeln sich Farben)
  void assignColors() {
    final keys = knownSubjects().map(subjectKey).toList();
    final used = <int>{
      for (final k in keys)
        if (subjectColors.containsKey(k)) subjectColors[k]!,
    };
    for (final k in keys) {
      if (subjectColors.containsKey(k)) continue;
      int idx = 0;
      while (used.contains(idx) && idx < subjectPaletteSize) {
        idx++;
      }
      if (idx >= subjectPaletteSize) idx = subjectColors.length % subjectPaletteSize;
      subjectColors[k] = idx;
      used.add(idx);
    }
  }

  // Farbindex eines Fachs (vor dem Speichern noch nicht vergeben: aus dem Namen)
  int colorIndexOf(String name) {
    final k = subjectKey(name);
    final i = subjectColors[k];
    if (i != null) return i % subjectPaletteSize;
    int h = 0;
    for (final c in k.codeUnits) {
      h = (h * 31 + c) & 0x7fffffff;
    }
    return h % subjectPaletteSize;
  }

  // --- Zeitraster ---------------------------------------------------------

  void sortSlots() => slots.sort((a, b) => a.startMin.compareTo(b.startMin));

  // Vorschlag für die nächste Rasterstunde: gleiche Länge und gleicher
  // Abstand wie zuletzt, sonst 08:00–08:45.
  Slot suggestNextSlot() {
    if (slots.isEmpty) return Slot(sh: 8, sm: 0, eh: 8, em: 45);
    final last = slots.last;
    final len = last.endMin - last.startMin;
    final gap = slots.length >= 2 ? last.startMin - slots[slots.length - 2].endMin : 0;
    final start = last.endMin + (gap > 0 ? gap : 0);
    final end = start + (len > 0 ? len : 45);
    return Slot(sh: start ~/ 60 % 24, sm: start % 60, eh: end ~/ 60 % 24, em: end % 60);
  }

  // Ältere Einträge ohne Rasterbezug zuordnen, wenn die Zeiten genau passen
  void linkPeriodsToSlots() {
    for (final list in days.values) {
      for (final p in list) {
        if (p.slot != null && p.slot! < slots.length) continue;
        p.slot = null;
        for (int i = 0; i < slots.length; i++) {
          final s = slots[i];
          if (s.sh == p.sh && s.sm == p.sm && s.eh == p.eh && s.em == p.em) {
            p.slot = i;
            break;
          }
        }
      }
    }
  }

  // Nach Änderungen am Raster: Zeiten der verknüpften Stunden nachziehen.
  // [oldSlots] ist das Raster vor der Änderung (gleiche Objekte, neu sortiert
  // oder mit gelöschten Einträgen).
  void applySlots(List<Slot> oldSlots) {
    for (final list in days.values) {
      for (final p in list) {
        if (p.slot == null || p.slot! >= oldSlots.length) {
          p.slot = null;
          continue;
        }
        final s = oldSlots[p.slot!];
        final i = slots.indexOf(s);
        if (i < 0) {
          p.slot = null; // Rasterstunde gelöscht: Eintrag bleibt mit seinen Zeiten
          continue;
        }
        p.slot = i;
        p.sh = s.sh;
        p.sm = s.sm;
        p.eh = s.eh;
        p.em = s.em;
      }
    }
  }

  // Eintrag eines Tages in Rasterstunde [slot] (oder null)
  Period? periodInSlot(int day, int slot) {
    for (final p in periodsFor(day)) {
      if (p.slot == slot) return p;
    }
    return null;
  }

  // Fach in Rasterstunde [slot] setzen; leerer Name löscht den Eintrag
  void setSlotSubject(int day, int slot, String name) {
    final list = periodsFor(day);
    final existing = periodInSlot(day, slot);
    if (name.trim().isEmpty) {
      if (existing != null) list.remove(existing);
      return;
    }
    final s = slots[slot];
    if (existing != null) {
      existing.name = name.trim();
    } else {
      list.add(Period(name: name.trim(), sh: s.sh, sm: s.sm, eh: s.eh, em: s.em, slot: slot));
    }
    assignColors();
  }

  // Alle bisher verwendeten Fächer (für Vorschläge), alphabetisch; Groß-/
  // Kleinschreibung zählt als dasselbe Fach (erste Schreibweise gewinnt)
  List<String> knownSubjects() {
    final byKey = <String, String>{};
    for (final list in days.values) {
      for (final p in list) {
        final n = p.name.trim();
        if (n.isNotEmpty) byKey.putIfAbsent(subjectKey(n), () => n);
      }
    }
    return byKey.values.toList()..sort((a, b) => a.toLowerCase().compareTo(b.toLowerCase()));
  }

  // --- Auswertung / Übertragung -------------------------------------------

  // Sortierte Perioden eines Tages (nach Startzeit)
  List<Period> sortedPeriods(int day) {
    final list = [...periodsFor(day)];
    list.sort((a, b) => (a.sh * 60 + a.sm).compareTo(b.sh * 60 + b.sm));
    return list;
  }

  // Wandelt DateTime.weekday (1=Mo..7=So) in die Tages-Konvention
  // der Uhr um (0=So, 1=Mo, ..., 6=Sa).
  static int weekdayOf(DateTime now) => now.weekday % 7;

  // Die Stunde, die zum Zeitpunkt [now] gerade läuft (oder null).
  Period? currentPeriod(DateTime now) {
    final cur = now.hour * 60 + now.minute;
    for (final p in sortedPeriods(weekdayOf(now))) {
      final s = p.sh * 60 + p.sm;
      final e = p.eh * 60 + p.em;
      if (cur >= s && cur < e) return p;
    }
    return null;
  }

  // Erzeugt die BLE-Befehle zum Übertragen des Stundenplans
  List<String> buildSendCommands() {
    final cmds = <String>['CLEAR'];
    for (int d = 0; d < 7; d++) {
      final sorted = sortedPeriods(d);
      for (int i = 0; i < sorted.length; i++) {
        final p = sorted[i];
        // '|' und Zeilenumbrueche wuerden das BLE-Protokoll der Uhr zerlegen.
        final safeName = p.name.replaceAll('|', ' ').replaceAll('\n', ' ').trim();
        // 9. Teil: Meldeziel pro Stunde (ältere Firmware liest ihn nicht)
        cmds.add('P|$d|$i|$safeName|${p.sh}|${p.sm}|${p.eh}|${p.em}|${goalOf(p.name)}');
      }
    }
    cmds.add('SAVE');
    return cmds;
  }
}
