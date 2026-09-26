import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';

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
              content: Column(
                mainAxisSize: MainAxisSize.min,
                children: [
                  TextField(
                    controller: nameCtrl,
                    decoration: const InputDecoration(labelText: "Fach / Name"),
                  ),
                  const SizedBox(height: 12),
                  Row(
                    children: [
                      Expanded(
                        child: OutlinedButton(
                          onPressed: () async {
                            final t = await showTimePicker(
                                context: ctx, initialTime: start);
                            if (t != null) setStateDialog(() => start = t);
                          },
                          child: Text(
                              "Start ${start.hour.toString().padLeft(2, '0')}:${start.minute.toString().padLeft(2, '0')}"),
                        ),
                      ),
                      const SizedBox(width: 8),
                      Expanded(
                        child: OutlinedButton(
                          onPressed: () async {
                            final t = await showTimePicker(
                                context: ctx, initialTime: end);
                            if (t != null) setStateDialog(() => end = t);
                          },
                          child: Text(
                              "Ende ${end.hour.toString().padLeft(2, '0')}:${end.minute.toString().padLeft(2, '0')}"),
                        ),
                      ),
                    ],
                  ),
                ],
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
      final p = Period(
        name: name,
        sh: start.hour,
        sm: start.minute,
        eh: end.hour,
        em: end.minute,
      );
      final list = widget.store.periodsFor(_day);
      if (existing != null) {
        final i = list.indexOf(existing);
        if (i >= 0) list[i] = p;
      } else {
        list.add(p);
      }
      await widget.store.save();
      if (mounted) setState(() {});
    }
  }

  Future<void> _deletePeriod(Period p) async {
    widget.store.periodsFor(_day).remove(p);
    await widget.store.save();
    if (mounted) setState(() {});
  }

  Future<void> _send() async {
    final tt = widget.ttChar;
    if (tt == null) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text("Bitte zuerst mit der Uhr verbinden.")),
      );
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
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text("Stundenplan übertragen (${cmds.length} Befehle).")),
        );
      }
    } catch (e) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text("Fehler beim Senden: $e")),
        );
      }
    } finally {
      if (mounted) setState(() => _sending = false);
    }
  }

  @override
  Widget build(BuildContext context) {
    final periods = widget.store.sortedPeriods(_day);
    return Scaffold(
      appBar: AppBar(
        title: const Text("Stundenplan"),
        actions: [
          IconButton(
            tooltip: "An die Uhr senden",
            onPressed: _sending ? null : _send,
            icon: _sending
                ? const SizedBox(
                    width: 20, height: 20, child: CircularProgressIndicator(strokeWidth: 2))
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
          Expanded(
            child: periods.isEmpty
                ? const Center(child: Text("Noch keine Stunden. Tippe auf +"))
                : ListView.builder(
                    itemCount: periods.length,
                    itemBuilder: (ctx, i) {
                      final p = periods[i];
                      return ListTile(
                        leading: const Icon(Icons.schedule),
                        title: Text(p.name),
                        subtitle: Text("${p.startLabel} – ${p.endLabel}"),
                        trailing: IconButton(
                          icon: const Icon(Icons.delete_outline),
                          onPressed: () => _deletePeriod(p),
                        ),
                        onTap: () => _editPeriod(existing: p),
                      );
                    },
                  ),
          ),
        ],
      ),
      floatingActionButton: FloatingActionButton(
        onPressed: () => _editPeriod(),
        child: const Icon(Icons.add),
      ),
    );
  }
}
