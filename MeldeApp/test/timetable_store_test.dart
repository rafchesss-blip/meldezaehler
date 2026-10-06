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
      store.periodsFor(1).add(Period(name: 'Mathe', sh: 8, sm: 15, eh: 9, em: 0));
      final cmds = store.buildSendCommands();
      expect(cmds, contains('P|1|0|Mathe|8|15|9|0|0'));
    });

    test('entfernt | und Zeilenumbrüche aus Fachnamen', () {
      final store = TimetableStore();
      store.periodsFor(1).add(Period(name: 'Mathe|Physik\nNeu', sh: 8, sm: 0, eh: 8, em: 45));
      final cmds = store.buildSendCommands();
      expect(cmds, contains('P|1|0|Mathe Physik Neu|8|0|8|45|0'));
      // Jeder Befehl hat genau 9 Teile (sonst würde das Protokoll zerlegt).
      for (final c in cmds.where((c) => c.startsWith('P|'))) {
        expect(c.split('|').length, 9);
      }
    });

    test('Tage werden einzeln, Indizes lückenlos übergeben', () {
      final store = TimetableStore();
      store.periodsFor(2).add(Period(name: 'Englisch', sh: 9, sm: 50, eh: 10, em: 35));
      final cmds = store.buildSendCommands();
      expect(cmds, contains('P|2|0|Englisch|9|50|10|35|0'));
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
      store.periodsFor(1).add(Period(name: 'Mathe', sh: 8, sm: 0, eh: 9, em: 0));
      // Montag 08:30 liegt in Mathe
      final p = store.currentPeriod(DateTime(2026, 9, 21, 8, 30));
      expect(p, isNotNull);
      expect(p!.name, 'Mathe');
    });

    test('currentPeriod liefert null außerhalb der Stunden', () {
      final store = TimetableStore();
      store.periodsFor(1).add(Period(name: 'Mathe', sh: 8, sm: 0, eh: 9, em: 0));
      expect(store.currentPeriod(DateTime(2026, 9, 21, 7, 59)), isNull);
      expect(store.currentPeriod(DateTime(2026, 9, 21, 9, 0)), isNull);
    });
  });

  group('Zeitraster', () {
    TimetableStore withSlots() {
      final store = TimetableStore();
      store.slots.addAll([Slot(sh: 8, sm: 0, eh: 8, em: 45), Slot(sh: 8, sm: 50, eh: 9, em: 35)]);
      return store;
    }

    test('suggestNextSlot: gleiche Länge und gleicher Abstand', () {
      final next = withSlots().suggestNextSlot();
      expect(next.startLabel, '09:40');
      expect(next.endLabel, '10:25');
    });

    test('suggestNextSlot ohne Raster: 08:00–08:45', () {
      final next = TimetableStore().suggestNextSlot();
      expect(next.startLabel, '08:00');
      expect(next.endLabel, '08:45');
    });

    test('setSlotSubject übernimmt die Rasterzeiten, leerer Name löscht', () {
      final store = withSlots();
      store.setSlotSubject(1, 1, 'Mathe');
      final p = store.periodInSlot(1, 1)!;
      expect(p.name, 'Mathe');
      expect(p.startLabel, '08:50');
      expect(p.endLabel, '09:35');
      store.setSlotSubject(1, 1, 'Deutsch');
      expect(store.periodsFor(1).length, 1);
      expect(store.periodInSlot(1, 1)!.name, 'Deutsch');
      store.setSlotSubject(1, 1, '  ');
      expect(store.periodsFor(1), isEmpty);
    });

    test('applySlots: geänderte Zeiten werden übernommen', () {
      final store = withSlots();
      store.setSlotSubject(2, 0, 'Bio');
      final old = [...store.slots];
      store.slots[0]
        ..sh = 7
        ..sm = 55
        ..eh = 8
        ..em = 40;
      store.sortSlots();
      store.applySlots(old);
      final p = store.periodInSlot(2, 0)!;
      expect(p.startLabel, '07:55');
      expect(p.endLabel, '08:40');
    });

    test('applySlots: Umsortieren und Löschen', () {
      final store = withSlots();
      store.setSlotSubject(3, 0, 'Kunst');
      store.setSlotSubject(3, 1, 'Musik');
      // neue Stunde vor allen anderen -> Indizes verschieben sich
      var old = [...store.slots];
      store.slots.add(Slot(sh: 7, sm: 0, eh: 7, em: 45));
      store.sortSlots();
      store.applySlots(old);
      expect(store.periodInSlot(3, 1)!.name, 'Kunst');
      expect(store.periodInSlot(3, 2)!.name, 'Musik');
      // Rasterstunde von Kunst löschen -> Eintrag bleibt, ohne Rasterbezug
      old = [...store.slots];
      store.slots.removeAt(1);
      store.applySlots(old);
      final kunst = store.periodsFor(3).firstWhere((p) => p.name == 'Kunst');
      expect(kunst.slot, isNull);
      expect(kunst.startLabel, '08:00');
      expect(store.periodInSlot(3, 1)!.name, 'Musik');
    });

    test('linkPeriodsToSlots ordnet alte Einträge mit passenden Zeiten zu', () {
      final store = withSlots();
      store.periodsFor(1).add(Period(name: 'Alt', sh: 8, sm: 50, eh: 9, em: 35));
      store.periodsFor(1).add(Period(name: 'AG', sh: 14, sm: 0, eh: 15, em: 0));
      store.linkPeriodsToSlots();
      expect(store.periodInSlot(1, 1)!.name, 'Alt');
      expect(store.periodsFor(1).firstWhere((p) => p.name == 'AG').slot, isNull);
    });

    test('Period-JSON mit und ohne slot', () {
      final a = Period.fromJson(Period(name: 'X', sh: 8, sm: 0, eh: 9, em: 0, slot: 2).toJson());
      expect(a.slot, 2);
      final b = Period.fromJson({'name': 'Y', 'sh': 8, 'sm': 0, 'eh': 9, 'em': 0});
      expect(b.slot, isNull);
    });

    test('knownSubjects: eindeutig und alphabetisch', () {
      final store = withSlots();
      store.setSlotSubject(1, 0, 'mathe');
      store.setSlotSubject(2, 0, 'Deutsch');
      store.setSlotSubject(3, 0, 'mathe');
      expect(store.knownSubjects(), ['Deutsch', 'mathe']);
    });
  });

  group('Fachfarben', () {
    test('gleiches Fach = gleiche Farbe, auch bei anderer Schreibweise', () {
      final store = TimetableStore();
      store.slots.add(Slot(sh: 8, sm: 0, eh: 8, em: 45));
      store.setSlotSubject(1, 0, 'Mathe');
      store.setSlotSubject(2, 0, 'mathe ');
      expect(store.colorIndexOf('Mathe'), store.colorIndexOf('MATHE'));
      expect(store.subjectColors.length, 1);
    });

    test('verschiedene Fächer bekommen verschiedene Farben', () {
      final store = TimetableStore();
      store.slots.add(Slot(sh: 8, sm: 0, eh: 8, em: 45));
      final names = ['Mathe', 'Deutsch', 'Englisch', 'Bio', 'Physik', 'Kunst'];
      for (int d = 0; d < names.length; d++) {
        store.setSlotSubject(d, 0, names[d]);
      }
      final idx = names.map(store.colorIndexOf).toSet();
      expect(idx.length, names.length);
    });

    test('Farbe bleibt fest, wenn neue Fächer dazukommen', () {
      final store = TimetableStore();
      store.slots.add(Slot(sh: 8, sm: 0, eh: 8, em: 45));
      store.setSlotSubject(1, 0, 'Mathe');
      final before = store.colorIndexOf('Mathe');
      store.setSlotSubject(2, 0, 'Algebra'); // alphabetisch davor
      store.setSlotSubject(3, 0, 'Biologie');
      expect(store.colorIndexOf('Mathe'), before);
    });
  });

  group('Fächer verwalten', () {
    TimetableStore plan() {
      final store = TimetableStore();
      store.slots.addAll([Slot(sh: 8, sm: 0, eh: 8, em: 45), Slot(sh: 8, sm: 50, eh: 9, em: 35)]);
      store.setSlotSubject(1, 0, 'Mathe');
      store.setSlotSubject(2, 1, 'mathe');
      store.setSlotSubject(1, 1, 'Deutsch');
      return store;
    }

    test('Meldeziel wird mitgesendet', () {
      final store = plan();
      store.setGoal('MATHE', 3);
      final cmds = store.buildSendCommands();
      expect(cmds, contains('P|1|0|Mathe|8|0|8|45|3'));
      expect(cmds, contains('P|2|0|mathe|8|50|9|35|3'));
      expect(cmds, contains('P|1|1|Deutsch|8|50|9|35|0'));
      store.setGoal('Mathe', 0);
      expect(store.goalOf('Mathe'), 0);
    });

    test('lessonsPerWeek zählt alle Schreibweisen', () {
      expect(plan().lessonsPerWeek('Mathe'), 2);
    });

    test('Umbenennen an allen Tagen, Farbe und Ziel bleiben', () {
      final store = plan();
      store.setGoal('Mathe', 2);
      final color = store.colorIndexOf('Mathe');
      store.renameSubject('Mathe', 'Mathematik');
      expect(store.periodInSlot(1, 0)!.name, 'Mathematik');
      expect(store.periodInSlot(2, 1)!.name, 'Mathematik');
      expect(store.goalOf('Mathematik'), 2);
      expect(store.colorIndexOf('Mathematik'), color);
      expect(store.goalOf('Mathe'), 0);
    });

    test('Umbenennen auf vorhandenes Fach legt zusammen', () {
      final store = plan();
      final deutschColor = store.colorIndexOf('Deutsch');
      store.renameSubject('Mathe', 'Deutsch');
      expect(store.knownSubjects(), ['Deutsch']);
      expect(store.colorIndexOf('Deutsch'), deutschColor);
      expect(store.lessonsPerWeek('Deutsch'), 3);
    });

    test('Löschen entfernt das Fach an allen Tagen', () {
      final store = plan();
      store.setGoal('Mathe', 4);
      store.deleteSubject('mathe');
      expect(store.knownSubjects(), ['Deutsch']);
      expect(store.goalOf('Mathe'), 0);
    });

    test('Farbe ändern', () {
      final store = plan();
      store.setColorIndex('Deutsch', 5);
      expect(store.colorIndexOf('deutsch'), 5);
    });
  });
}
