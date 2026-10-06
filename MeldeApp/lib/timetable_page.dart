import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';

import 'slots_page.dart';
import 'subject_colors.dart';
import 'subjects_page.dart';
import 'timetable_store.dart';

class TimetablePage extends StatefulWidget {
  final TimetableStore store;
  final BluetoothCharacteristic? ttChar;

  const TimetablePage({super.key, required this.store, this.ttChar});

  @override
  State<TimetablePage> createState() => _TimetablePageState();
}

class _TimetablePageState extends State<TimetablePage> {
  int _day = 1; // Mo
  bool _sending = false;

  @override
  void initState() {
    super.initState();
    widget.store.load().then((_) {
      if (mounted) setState(() {});
    });
  }

  Future<void> _editPeriod({Period? existing}) async {
    final nameCtrl = TextEditingController(text: existing?.name ?? '');
    TimeOfDay start = existing != null
        ? TimeOfDay(hour: existing.sh, minute: existing.sm)
        : const TimeOfDay(hour: 8, minute: 0);
    TimeOfDay end = existing != null
        ? TimeOfDay(hour: existing.eh, minute: existing.em)
        : const TimeOfDay(hour: 8, minute: 45);

    final result = await showDialog<bool>(
      context: context,
      builder: (ctx) {
        return StatefulBuilder(
          builder: (ctx, setStateDialog) {
            return AlertDialog(
              title: Text(existing == null ? "Stunde hinzufügen" : "Stunde bearbeiten"),
              content: SingleChildScrollView(
                child: Column(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    SubjectField(store: widget.store, controller: nameCtrl),
                    const SizedBox(height: 12),
                    Row(
                      children: [
                        Expanded(
                          child: OutlinedButton(
                            onPressed: () async {
                              final t = await showTimePicker(context: ctx, initialTime: start);
                              if (t != null) setStateDialog(() => start = t);
                            },
                            child: Text(
                              "Start ${start.hour.toString().padLeft(2, '0')}:${start.minute.toString().padLeft(2, '0')}",
                            ),
                          ),
                        ),
                        const SizedBox(width: 8),
                        Expanded(
                          child: OutlinedButton(
                            onPressed: () async {
                              final t = await showTimePicker(context: ctx, initialTime: end);
                              if (t != null) setStateDialog(() => end = t);
                            },
                            child: Text(
                              "Ende ${end.hour.toString().padLeft(2, '0')}:${end.minute.toString().padLeft(2, '0')}",
                            ),
                          ),
                        ),
                      ],
                    ),
                  ],
                ),
              ),
              actions: [
                TextButton(
                  onPressed: () => Navigator.pop(ctx, false),
                  child: const Text("Abbrechen"),
                ),
                FilledButton(
                  onPressed: () {
                    final name = nameCtrl.text.trim();
                    if (name.isEmpty) return;
                    Navigator.pop(ctx, true);
                  },
                  child: const Text("Übernehmen"),
                ),
              ],
            );
          },
        );
      },
    );

    if (result == true) {
      final name = nameCtrl.text.trim();
      final p = Period(name: name, sh: start.hour, sm: start.minute, eh: end.hour, em: end.minute);
      final list = widget.store.periodsFor(_day);
      if (existing != null) {
        final i = list.indexOf(existing);
        if (i >= 0) list[i] = p;
      } else {
        list.add(p);
      }
      widget.store.assignColors();
      await widget.store.save();
      if (mounted) setState(() {});
    }
  }

  // Fach für eine Rasterstunde eintragen (mit Vorschlägen bereits benutzter Fächer)
  Future<void> _editSubject(int slot) async {
    final s = widget.store.slots[slot];
    final existing = widget.store.periodInSlot(_day, slot);
    final ctrl = TextEditingController(text: existing?.name ?? '');

    final result = await showDialog<String>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: Text("${dayNames[_day]}, ${slot + 1}. Stunde"),
        content: SingleChildScrollView(
          child: Column(
            mainAxisSize: MainAxisSize.min,
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Text("${s.startLabel} – ${s.endLabel}"),
              const SizedBox(height: 8),
              // Antippen eines bisherigen Fachs übernimmt es sofort
              SubjectField(
                store: widget.store,
                controller: ctrl,
                onPicked: (v) => Navigator.pop(ctx, v),
                onSubmitted: (v) => Navigator.pop(ctx, v),
              ),
            ],
          ),
        ),
        actions: [
          if (existing != null)
            TextButton(onPressed: () => Navigator.pop(ctx, ''), child: const Text("Leeren")),
          TextButton(onPressed: () => Navigator.pop(ctx), child: const Text("Abbrechen")),
          FilledButton(
            onPressed: () => Navigator.pop(ctx, ctrl.text),
            child: const Text("Übernehmen"),
          ),
        ],
      ),
    );
    if (result == null) return;
    widget.store.setSlotSubject(_day, slot, result);
    await widget.store.save();
    if (mounted) setState(() {});
  }

  Future<void> _openSlots() async {
    await Navigator.of(context)
        .push(MaterialPageRoute(builder: (_) => SlotsPage(store: widget.store)));
    if (mounted) setState(() {});
  }

  Future<void> _openSubjects() async {
    await Navigator.of(context)
        .push(MaterialPageRoute(builder: (_) => SubjectsPage(store: widget.store)));
    if (mounted) setState(() {});
  }

  // Zeit der Stunde, dazu das Meldeziel des Fachs (falls gesetzt)
  String _timeLine(String start, String end, String? subject) {
    final goal = subject == null ? 0 : widget.store.goalOf(subject);
    return goal > 0 ? "$start – $end  ·  Ziel $goal" : "$start – $end";
  }

  // Zeilen eines Tages: alle Rasterstunden (belegt oder frei), dazu freie
  // Einträge außerhalb des Rasters – nach Startzeit sortiert
  List<Widget> _dayRows() {
    final store = widget.store;
    final rows = <MapEntry<int, Widget>>[];
    for (int i = 0; i < store.slots.length; i++) {
      final s = store.slots[i];
      final p = store.periodInSlot(_day, i);
      final color = p == null ? null : subjectColor(store, p.name);
      rows.add(
        MapEntry(
          s.startMin,
          ListTile(
            leading: CircleAvatar(
              backgroundColor: color ?? Colors.transparent,
              foregroundColor: color == null ? null : onSubjectColor(color),
              child: Text("${i + 1}"),
            ),
            tileColor: color?.withValues(alpha: 0.12),
            title: p == null
                ? Text("frei", style: TextStyle(color: Theme.of(context).disabledColor))
                : Text(p.name),
            subtitle: Text(_timeLine(s.startLabel, s.endLabel, p?.name)),
            trailing: p == null
                ? const Icon(Icons.edit_outlined)
                : IconButton(
                    icon: const Icon(Icons.delete_outline),
                    onPressed: () => _deletePeriod(p),
                  ),
            onTap: () => _editSubject(i),
          ),
        ),
      );
    }
    for (final p in store.periodsFor(_day)) {
      if (p.slot != null) continue;
      rows.add(
        MapEntry(
          p.sh * 60 + p.sm,
          ListTile(
            leading: Icon(Icons.schedule, color: subjectColor(store, p.name)),
            tileColor: subjectColor(store, p.name).withValues(alpha: 0.12),
            title: Text(p.name),
            subtitle: Text(_timeLine(p.startLabel, p.endLabel, p.name)),
            trailing: IconButton(
              icon: const Icon(Icons.delete_outline),
              onPressed: () => _deletePeriod(p),
            ),
            onTap: () => _editPeriod(existing: p),
          ),
        ),
      );
    }
    rows.sort((a, b) => a.key.compareTo(b.key));
    return rows.map((e) => e.value).toList();
  }

  Future<void> _deletePeriod(Period p) async {
    widget.store.periodsFor(_day).remove(p);
    await widget.store.save();
    if (mounted) setState(() {});
  }

  Future<void> _send() async {
    final tt = widget.ttChar;
    if (tt == null) {
      ScaffoldMessenger.of(context)
          .showSnackBar(const SnackBar(content: Text("Bitte zuerst mit der Uhr verbinden.")));
      return;
    }
    setState(() => _sending = true);
    try {
      final cmds = widget.store.buildSendCommands();
      for (final c in cmds) {
        await tt.write(utf8.encode(c), withoutResponse: false);
        await Future.delayed(const Duration(milliseconds: 60));
      }
      if (mounted) {
        ScaffoldMessenger.of(
          context,
        ).showSnackBar(SnackBar(content: Text("Stundenplan übertragen (${cmds.length} Befehle).")));
      }
    } catch (e) {
      if (mounted) {
        ScaffoldMessenger.of(context)
            .showSnackBar(SnackBar(content: Text("Fehler beim Senden: $e")));
      }
    } finally {
      if (mounted) setState(() => _sending = false);
    }
  }

  @override
  Widget build(BuildContext context) {
    final rows = _dayRows();
    return Scaffold(
      appBar: AppBar(
        title: const Text("Stundenplan"),
        actions: [
          IconButton(
            tooltip: "Fächer & Meldeziele",
            onPressed: _openSubjects,
            icon: const Icon(Icons.school_outlined),
          ),
          IconButton(
            tooltip: "Zeitraster",
            onPressed: _openSlots,
            icon: const Icon(Icons.view_agenda_outlined),
          ),
          IconButton(
            tooltip: "An die Uhr senden",
            onPressed: _sending ? null : _send,
            icon: _sending
                ? const SizedBox(
                    width: 20,
                    height: 20,
                    child: CircularProgressIndicator(strokeWidth: 2),
                  )
                : Icon(widget.ttChar != null ? Icons.upload : Icons.cloud_off),
          ),
        ],
      ),
      body: Column(
        children: [
          SizedBox(
            height: 44,
            child: ListView.builder(
              scrollDirection: Axis.horizontal,
              itemCount: 7,
              itemBuilder: (ctx, i) {
                final selected = i == _day;
                return Padding(
                  padding: const EdgeInsets.symmetric(horizontal: 3),
                  child: ChoiceChip(
                    label: Text(dayNames[i]),
                    selected: selected,
                    onSelected: (_) => setState(() => _day = i),
                  ),
                );
              },
            ),
          ),
          const Divider(height: 1),
          if (widget.store.slots.isEmpty)
            Card(
              margin: const EdgeInsets.all(12),
              child: ListTile(
                leading: const Icon(Icons.view_agenda_outlined),
                title: const Text("Zeitraster anlegen"),
                subtitle: const Text(
                  "Einmal festlegen, wann welche Stunde ist – dann nur noch Fächer eintragen.",
                ),
                trailing: const Icon(Icons.chevron_right),
                onTap: _openSlots,
              ),
            ),
          Expanded(
            child: rows.isEmpty
                ? const Center(child: Text("Noch keine Stunden. Tippe auf +"))
                : ListView(children: rows),
          ),
        ],
      ),
      // + : Stunde außerhalb des Rasters (z. B. AG, Nachmittag)
      floatingActionButton: FloatingActionButton(
        tooltip: "Stunde außerhalb des Rasters",
        onPressed: () => _editPeriod(),
        child: const Icon(Icons.add),
      ),
    );
  }
}
