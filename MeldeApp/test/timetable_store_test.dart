// Unit-Tests für TimetableStore: Sortierung, BLE-Befehle und
// Bereinigung von Protokoll-Trennzeichen in Fachnamen.

import 'package:flutter_test/flutter_test.dart';

import 'package:melde_app/timetable_store.dart';

void main() {
  group('Period', () {
    test('JSON-Roundtrip', () {
      final p = Period(name: 'Mathe', sh: 8, sm: 15, eh: 9, em: 0);
      final restored = Period.fromJson(p.toJson());
      expect(restored.name, 'Mathe');
      expect(restored.sh, 8);
      expect(restored.sm, 15);
      expect(restored.eh, 9);
      expect(restored.em, 0);
    });

    test('startLabel/endLabel mit führenden Nullen', () {
      final p = Period(name: 'Deutsch', sh: 8, sm: 5, eh: 9, em: 45);
      expect(p.startLabel, '08:05');
      expect(p.endLabel, '09:45');
    });
  });

  group('TimetableStore.sortedPeriods', () {
    test('sortiert nach Startzeit', () {
      final store = TimetableStore();
      store.periodsFor(1).addAll([
        Period(name: 'Spät', sh: 11, sm: 30, eh: 12, em: 15),
        Period(name: 'Früh', sh: 8, sm: 0, eh: 8, em: 45),
        Period(name: 'Mittel', sh: 9, sm: 50, eh: 10, em: 35),
      ]);
      final sorted = store.sortedPeriods(1);
      expect(sorted.map((p) => p.name).toList(), ['Früh', 'Mittel', 'Spät']);
    });
  });

  group('TimetableStore.buildSendCommands', () {
    test('beginnt mit CLEAR und endet mit SAVE', () {
      final store = TimetableStore();
      final cmds = store.buildSendCommands();
      expect(cmds.first, 'CLEAR');
      expect(cmds.last, 'SAVE');
    });

    test('erzeugt korrektes P-Kommando', () {
      final store = TimetableStore();
      store.periodsFor(1).add(
            Period(name: 'Mathe', sh: 8, sm: 15, eh: 9, em: 0),
          );
      final cmds = store.buildSendCommands();
      expect(cmds, contains('P|1|0|Mathe|8|15|9|0'));
    });

    test('entfernt | und Zeilenumbrüche aus Fachnamen', () {
      final store = TimetableStore();
      store.periodsFor(1).add(
            Period(name: 'Mathe|Physik\nNeu', sh: 8, sm: 0, eh: 8, em: 45),
          );
      final cmds = store.buildSendCommands();
      expect(cmds, contains('P|1|0|Mathe Physik Neu|8|0|8|45'));
      // Kein Befehl darf mehr als 8 Teile haben (würde das Protokoll zerlegen).
      for (final c in cmds.where((c) => c.startsWith('P|'))) {
        expect(c.split('|').length, lessThanOrEqualTo(8));
      }
    });

    test('Tage werden einzeln, Indizes lückenlos übergeben', () {
      final store = TimetableStore();
      store.periodsFor(2).add(
            Period(name: 'Englisch', sh: 9, sm: 50, eh: 10, em: 35),
          );
      final cmds = store.buildSendCommands();
      expect(cmds, contains('P|2|0|Englisch|9|50|10|35'));
    });
  });

  group('TimetableStore.weekdayOf / currentPeriod', () {
    test('weekdayOf bildet auf 0=So..6=Sa ab', () {
      expect(TimetableStore.weekdayOf(DateTime(2026, 9, 21)), 1); // Montag
      expect(TimetableStore.weekdayOf(DateTime(2026, 9, 27)), 0); // Sonntag
      expect(TimetableStore.weekdayOf(DateTime(2026, 9, 26)), 6); // Samstag
    });

    test('currentPeriod findet die laufende Stunde', () {
      final store = TimetableStore();
      store.periodsFor(1).add(
            Period(name: 'Mathe', sh: 8, sm: 0, eh: 9, em: 0),
          );
      // Montag 08:30 liegt in Mathe
      final p = store.currentPeriod(DateTime(2026, 9, 21, 8, 30));
      expect(p, isNotNull);
      expect(p!.name, 'Mathe');
    });

    test('currentPeriod liefert null außerhalb der Stunden', () {
      final store = TimetableStore();
      store.periodsFor(1).add(
            Period(name: 'Mathe', sh: 8, sm: 0, eh: 9, em: 0),
          );
      expect(store.currentPeriod(DateTime(2026, 9, 21, 7, 59)), isNull);
      expect(store.currentPeriod(DateTime(2026, 9, 21, 9, 0)), isNull);
    });
  });
}
