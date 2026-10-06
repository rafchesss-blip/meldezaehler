import 'package:flutter/material.dart';

import 'timetable_store.dart';

// Zeitraster bearbeiten: wann ist die 1., 2., 3. ... Stunde. Gilt für alle
// Tage; Stunden im Stundenplan, die einer Rasterstunde zugeordnet sind,
// übernehmen geänderte Zeiten automatisch.
class SlotsPage extends StatefulWidget {
  final TimetableStore store;

  const SlotsPage({super.key, required this.store});

  @override
  State<SlotsPage> createState() => _SlotsPageState();
}

class _SlotsPageState extends State<SlotsPage> {
  String _fmt(TimeOfDay t) =>
      '${t.hour.toString().padLeft(2, '0')}:${t.minute.toString().padLeft(2, '0')}';

  // Änderung am Raster anwenden: sortieren, Stundenplan nachziehen, speichern
  Future<void> _commit(List<Slot> oldSlots) async {
    widget.store.sortSlots();
    widget.store.applySlots(oldSlots);
    await widget.store.save();
    if (mounted) setState(() {});
  }

  Future<void> _edit({Slot? existing}) async {
    final proposal = existing ?? widget.store.suggestNextSlot();
    TimeOfDay start = TimeOfDay(hour: proposal.sh, minute: proposal.sm);
    TimeOfDay end = TimeOfDay(hour: proposal.eh, minute: proposal.em);

    final ok = await showDialog<bool>(
      context: context,
      builder: (ctx) => StatefulBuilder(
        builder: (ctx, setStateDialog) => AlertDialog(
          title: Text(existing == null
              ? "${widget.store.slots.length + 1}. Stunde hinzufügen"
              : "${widget.store.slots.indexOf(existing) + 1}. Stunde bearbeiten"),
          content: Row(
            children: [
              Expanded(
                child: OutlinedButton(
                  onPressed: () async {
                    final t = await showTimePicker(context: ctx, initialTime: start);
                    if (t == null) return;
                    // Ende um die gleiche Dauer mitschieben
                    final len = (end.hour * 60 + end.minute) - (start.hour * 60 + start.minute);
                    final e = t.hour * 60 + t.minute + (len > 0 ? len : 45);
                    setStateDialog(() {
                      start = t;
                      end = TimeOfDay(hour: e ~/ 60 % 24, minute: e % 60);
                    });
                  },
                  child: Text("Von ${_fmt(start)}"),
                ),
              ),
              const SizedBox(width: 8),
              Expanded(
                child: OutlinedButton(
                  onPressed: () async {
                    final t = await showTimePicker(context: ctx, initialTime: end);
                    if (t != null) setStateDialog(() => end = t);
                  },
                  child: Text("Bis ${_fmt(end)}"),
                ),
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
                if (end.hour * 60 + end.minute <= start.hour * 60 + start.minute) return;
                Navigator.pop(ctx, true);
              },
              child: const Text("Übernehmen"),
            ),
          ],
        ),
      ),
    );
    if (ok != true) return;

    final oldSlots = [...widget.store.slots];
    if (existing != null) {
      existing
        ..sh = start.hour
        ..sm = start.minute
        ..eh = end.hour
        ..em = end.minute;
    } else {
      widget.store.slots
          .add(Slot(sh: start.hour, sm: start.minute, eh: end.hour, em: end.minute));
    }
    await _commit(oldSlots);
  }

  Future<void> _delete(Slot s) async {
    final oldSlots = [...widget.store.slots];
    widget.store.slots.remove(s);
    await _commit(oldSlots);
  }

  @override
  Widget build(BuildContext context) {
    final slots = widget.store.slots;
    return Scaffold(
      appBar: AppBar(title: const Text("Zeitraster")),
      body: Column(
        children: [
          const Padding(
            padding: EdgeInsets.fromLTRB(16, 12, 16, 4),
            child: Text(
              "Einmal festlegen, wann welche Stunde ist – gilt für alle Tage. "
              "Im Stundenplan trägst du danach nur noch die Fächer ein.",
            ),
          ),
          Expanded(
            child: slots.isEmpty
                ? const Center(child: Text("Noch kein Raster. Tippe auf +"))
                : ListView.builder(
                    itemCount: slots.length,
                    itemBuilder: (ctx, i) {
                      final s = slots[i];
                      return ListTile(
                        leading: CircleAvatar(child: Text("${i + 1}")),
                        title: Text("${i + 1}. Stunde"),
                        subtitle: Text("${s.startLabel} – ${s.endLabel}"),
                        trailing: IconButton(
                          icon: const Icon(Icons.delete_outline),
                          onPressed: () => _delete(s),
                        ),
                        onTap: () => _edit(existing: s),
                      );
                    },
                  ),
          ),
        ],
      ),
      floatingActionButton: FloatingActionButton.extended(
        onPressed: () => _edit(),
        icon: const Icon(Icons.add),
        label: const Text("Stunde"),
      ),
    );
  }
}
