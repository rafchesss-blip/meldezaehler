import 'dart:convert';

import 'package:shared_preferences/shared_preferences.dart';

// Tages-Konvention wie auf der Uhr: 0=So, 1=Mo, 2=Di, 3=Mi, 4=Do, 5=Fr, 6=Sa
const List<String> dayNames = ['So', 'Mo', 'Di', 'Mi', 'Do', 'Fr', 'Sa'];

class Period {
  String name;
  int sh, sm, eh, em; // Start/Ende (Stunde, Minute)

  Period({
    required this.name,
    required this.sh,
    required this.sm,
    required this.eh,
    required this.em,
  });

  Map<String, dynamic> toJson() =>
      {'name': name, 'sh': sh, 'sm': sm, 'eh': eh, 'em': em};

  factory Period.fromJson(Map<String, dynamic> j) => Period(
        name: (j['name'] ?? '').toString(),
        sh: (j['sh'] ?? 0) as int,
        sm: (j['sm'] ?? 0) as int,
        eh: (j['eh'] ?? 0) as int,
        em: (j['em'] ?? 0) as int,
      );

  String get startLabel =>
      '${sh.toString().padLeft(2, '0')}:${sm.toString().padLeft(2, '0')}';
  String get endLabel =>
      '${eh.toString().padLeft(2, '0')}:${em.toString().padLeft(2, '0')}';
}

class TimetableStore {
  final Map<int, List<Period>> days = {};

  List<Period> periodsFor(int day) => days.putIfAbsent(day, () => []);

  Future<void> load() async {
    final prefs = await SharedPreferences.getInstance();
    final s = prefs.getString('timetable');
    days.clear();
    if (s != null && s.isNotEmpty) {
      try {
        final map = jsonDecode(s) as Map<String, dynamic>;
        map.forEach((k, v) {
          days[int.parse(k)] =
              (v as List).map((e) => Period.fromJson(e as Map<String, dynamic>)).toList();
        });
      } catch (_) {}
    }
  }

  Future<void> save() async {
    final prefs = await SharedPreferences.getInstance();
    final map = <String, dynamic>{};
    days.forEach((k, v) {
      map['$k'] = v.map((e) => e.toJson()).toList();
    });
    await prefs.setString('timetable', jsonEncode(map));
  }

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
        final safeName =
            p.name.replaceAll('|', ' ').replaceAll('\n', ' ').trim();
        cmds.add('P|$d|$i|$safeName|${p.sh}|${p.sm}|${p.eh}|${p.em}');
      }
    }
    cmds.add('SAVE');
    return cmds;
  }
}
