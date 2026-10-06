import 'package:flutter/material.dart';

import 'timetable_store.dart';

// Fachfarben: gut unterscheidbar, in hellem und dunklem Design lesbar.
// Länge muss subjectPaletteSize entsprechen.
const List<Color> subjectPalette = [
  Color(0xFFE53935), // Rot
  Color(0xFF1E88E5), // Blau
  Color(0xFF43A047), // Grün
  Color(0xFFFB8C00), // Orange
  Color(0xFF8E24AA), // Lila
  Color(0xFF00ACC1), // Cyan
  Color(0xFFD81B60), // Pink
  Color(0xFF6D4C41), // Braun
  Color(0xFF3949AB), // Indigo
  Color(0xFFC0CA33), // Limette
  Color(0xFF00897B), // Petrol
  Color(0xFFFFB300), // Bernstein
  Color(0xFF546E7A), // Blaugrau
  Color(0xFF5E35B1), // Violett
];

Color subjectColor(TimetableStore store, String name) =>
    subjectPalette[store.colorIndexOf(name) % subjectPalette.length];

// Textfarbe auf der Fachfarbe
Color onSubjectColor(Color c) => c.computeLuminance() > 0.45 ? Colors.black87 : Colors.white;

// Eingabefeld für ein Fach mit den bisherigen Fächern als antippbare Chips
// darunter (beim Tippen gefiltert). [onPicked] wird beim Antippen eines Chips
// aufgerufen.
class SubjectField extends StatefulWidget {
  final TimetableStore store;
  final TextEditingController controller;
  final ValueChanged<String>? onPicked;
  final ValueChanged<String>? onSubmitted;

  const SubjectField({
    super.key,
    required this.store,
    required this.controller,
    this.onPicked,
    this.onSubmitted,
  });

  @override
  State<SubjectField> createState() => _SubjectFieldState();
}

class _SubjectFieldState extends State<SubjectField> {
  late final List<String> _subjects = widget.store.knownSubjects();

  @override
  Widget build(BuildContext context) {
    final q = widget.controller.text.trim().toLowerCase();
    final shown = q.isEmpty
        ? _subjects
        : _subjects.where((e) => e.toLowerCase().contains(q) && e.toLowerCase() != q).toList();
    return Column(
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        TextField(
          controller: widget.controller,
          autofocus: _subjects.isEmpty,
          textCapitalization: TextCapitalization.sentences,
          decoration: InputDecoration(
            labelText: "Fach",
            prefixIcon: q.isEmpty
                ? null
                : Padding(
                    padding: const EdgeInsets.all(12),
                    child: CircleAvatar(
                      radius: 8,
                      backgroundColor: subjectColor(widget.store, widget.controller.text),
                    ),
                  ),
          ),
          onChanged: (_) => setState(() {}),
          onSubmitted: widget.onSubmitted,
        ),
        if (shown.isNotEmpty) ...[
          const SizedBox(height: 12),
          ConstrainedBox(
            constraints: const BoxConstraints(maxHeight: 180),
            child: SingleChildScrollView(
              child: Wrap(
                spacing: 6,
                runSpacing: 6,
                children: [
                  for (final s in shown)
                    ActionChip(
                      avatar: CircleAvatar(backgroundColor: subjectColor(widget.store, s)),
                      label: Text(s),
                      onPressed: () {
                        widget.controller.text = s;
                        setState(() {});
                        widget.onPicked?.call(s);
                      },
                    ),
                ],
              ),
            ),
          ),
        ],
      ],
    );
  }
}
