import 'package:flutter/material.dart';

import 'subject_colors.dart';
import 'timetable_store.dart';

// Fächer verwalten: umbenennen (an allen Tagen), Farbe, Meldeziel pro Stunde,
// löschen. Das Meldeziel zeigt die Uhr während der Stunde auf jedem
// Zifferblatt an.
class SubjectsPage extends StatefulWidget {
  final TimetableStore store;

  const SubjectsPage({super.key, required this.store});

  @override
  State<SubjectsPage> createState() => _SubjectsPageState();
}

class _SubjectsPageState extends State<SubjectsPage> {
  Future<void> _edit(String subject) async {
    final store = widget.store;
    final nameCtrl = TextEditingController(text: subject);
    int color = store.colorIndexOf(subject);
    int goal = store.goalOf(subject);

    final action = await showDialog<String>(
      context: context,
      builder: (ctx) => StatefulBuilder(
        builder: (ctx, setStateDialog) => AlertDialog(
          title: const Text("Fach bearbeiten"),
          content: SingleChildScrollView(
            child: Column(
              mainAxisSize: MainAxisSize.min,
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                TextField(
                  controller: nameCtrl,
                  textCapitalization: TextCapitalization.sentences,
                  decoration: const InputDecoration(labelText: "Name"),
                ),
                const SizedBox(height: 20),
                const Text("Meldeziel pro Stunde"),
                const SizedBox(height: 4),
                Row(
                  children: [
                    IconButton.outlined(
                      onPressed: goal > 0 ? () => setStateDialog(() => goal--) : null,
                      icon: const Icon(Icons.remove),
                    ),
                    Expanded(
                      child: Text(
                        goal == 0 ? "kein Ziel" : "$goal Meldung${goal == 1 ? '' : 'en'}",
                        textAlign: TextAlign.center,
                        style: Theme.of(ctx).textTheme.titleMedium,
                      ),
                    ),
                    IconButton.outlined(
                      onPressed: goal < 99 ? () => setStateDialog(() => goal++) : null,
                      icon: const Icon(Icons.add),
                    ),
                  ],
                ),
                const SizedBox(height: 20),
                const Text("Farbe"),
                const SizedBox(height: 8),
                Wrap(
                  spacing: 8,
                  runSpacing: 8,
                  children: [
                    for (int i = 0; i < subjectPalette.length; i++)
                      InkWell(
                        customBorder: const CircleBorder(),
                        onTap: () => setStateDialog(() => color = i),
                        child: CircleAvatar(
                          radius: 16,
                          backgroundColor: subjectPalette[i],
                          child: i == color
                              ? Icon(
                                  Icons.check,
                                  size: 18,
                                  color: onSubjectColor(subjectPalette[i]),
                                )
                              : null,
                        ),
                      ),
                  ],
                ),
              ],
            ),
          ),
          actions: [
            TextButton(
              onPressed: () => Navigator.pop(ctx, 'delete'),
              style: TextButton.styleFrom(foregroundColor: Theme.of(ctx).colorScheme.error),
              child: const Text("Löschen"),
            ),
            TextButton(onPressed: () => Navigator.pop(ctx), child: const Text("Abbrechen")),
            FilledButton(
              onPressed: () {
                if (nameCtrl.text.trim().isEmpty) return;
                Navigator.pop(ctx, 'save');
              },
              child: const Text("Speichern"),
            ),
          ],
        ),
      ),
    );

    if (action == 'delete') {
      if (!mounted) return;
      final n = store.lessonsPerWeek(subject);
      final ok = await showDialog<bool>(
        context: context,
        builder: (ctx) => AlertDialog(
          title: Text("„$subject“ löschen?"),
          content: Text("Das Fach wird aus allen $n Stunden im Stundenplan entfernt."),
          actions: [
            TextButton(onPressed: () => Navigator.pop(ctx, false), child: const Text("Abbrechen")),
            FilledButton(onPressed: () => Navigator.pop(ctx, true), child: const Text("Löschen")),
          ],
        ),
      );
      if (ok != true) return;
      store.deleteSubject(subject);
    } else if (action == 'save') {
      final newName = nameCtrl.text.trim();
      store.renameSubject(subject, newName);
      store.setColorIndex(newName, color);
      store.setGoal(newName, goal);
    } else {
      return;
    }
    await store.save();
    if (mounted) setState(() {});
  }

  @override
  Widget build(BuildContext context) {
    final store = widget.store;
    final subjects = store.knownSubjects();
    return Scaffold(
      appBar: AppBar(title: const Text("Fächer")),
      body: subjects.isEmpty
          ? const Center(
              child: Padding(
                padding: EdgeInsets.all(24),
                child: Text(
                  "Noch keine Fächer. Trag im Stundenplan Fächer ein – sie erscheinen dann hier.",
                  textAlign: TextAlign.center,
                ),
              ),
            )
          : ListView.builder(
              itemCount: subjects.length,
              itemBuilder: (ctx, i) {
                final s = subjects[i];
                final c = subjectColor(store, s);
                final n = store.lessonsPerWeek(s);
                final goal = store.goalOf(s);
                return ListTile(
                  leading: CircleAvatar(
                    backgroundColor: c,
                    foregroundColor: onSubjectColor(c),
                    child: Text(s.characters.first.toUpperCase()),
                  ),
                  title: Text(s),
                  subtitle: Text("$n Stunde${n == 1 ? '' : 'n'} pro Woche"),
                  trailing: goal > 0
                      ? Chip(
                          avatar: const Icon(Icons.flag_outlined, size: 18),
                          label: Text("Ziel $goal"),
                        )
                      : const Text("kein Ziel"),
                  onTap: () => _edit(s),
                );
              },
            ),
    );
  }
}
