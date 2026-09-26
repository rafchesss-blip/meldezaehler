import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:permission_handler/permission_handler.dart';

import 'timetable_page.dart';
import 'timetable_store.dart';

const String serviceUuid = "4fafc201-1fb5-459e-8fcc-c5c9c331914b";
const String charTimeUuid = "beb5483e-36e1-4688-b7f5-ea07361b26a8";
const String charStatsUuid = "beb5483e-36e1-4688-b7f5-ea07361b26a9";
const String charClearUuid = "beb5483e-36e1-4688-b7f5-ea07361b26aa";
const String charTtUuid = "beb5483e-36e1-4688-b7f5-ea07361b26ab";
const String charLessonUuid = "beb5483e-36e1-4688-b7f5-ea07361b26ac";

void main() {
  WidgetsFlutterBinding.ensureInitialized();
  runApp(const MeldeApp());
}

class MeldeApp extends StatelessWidget {
  const MeldeApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Meldezähler',
      debugShowCheckedModeBanner: false,
      theme: ThemeData(colorSchemeSeed: Colors.teal, useMaterial3: true),
      home: const HomePage(),
    );
  }
}

class HomePage extends StatefulWidget {
  const HomePage({super.key});

  @override
  State<HomePage> createState() => _HomePageState();
}

class _HomePageState extends State<HomePage> {
  BluetoothDevice? _device;
  BluetoothCharacteristic? _timeChar;
  BluetoothCharacteristic? _statsChar;
  BluetoothCharacteristic? _clearChar;
  BluetoothCharacteristic? _ttChar;
  BluetoothCharacteristic? _lessonChar;

  bool _busy = false;
  bool _connected = false;
  String _status = "Bereit";
  Map<String, dynamic>? _stats;

  final TimetableStore _ttStore = TimetableStore();

  @override
  void initState() {
    super.initState();
    _ttStore.load().then((_) {
      if (mounted) setState(() {});
    });
  }

  Future<void> _ensurePermissions() async {
    await [Permission.bluetoothScan, Permission.bluetoothConnect].request();
    try {
      if (await Permission.locationWhenInUse.isDenied) {
        await Permission.locationWhenInUse.request();
      }
    } catch (_) {}
  }

  bool _isOurDevice(ScanResult r) {
    final name = r.advertisementData.advName;
    final hasUuid = r.advertisementData.serviceUuids.any((g) => g.str == serviceUuid);
    return hasUuid || name.contains("Melde");
  }

  Future<void> _connect() async {
    setState(() {
      _busy = true;
      _connected = false;
      _status = "Prüfe Berechtigungen ...";
    });

    try {
      await _ensurePermissions();
      await FlutterBluePlus.turnOn();

      setState(() => _status = "Suche Meldezaehler ...");

      await FlutterBluePlus.startScan(timeout: const Duration(seconds: 15));

      BluetoothDevice? found;
      final deadline = DateTime.now().add(const Duration(seconds: 16));
      while (DateTime.now().isBefore(deadline)) {
        for (final r in FlutterBluePlus.lastScanResults) {
          if (_isOurDevice(r)) {
            found = r.device;
            break;
          }
        }
        if (found != null) break;
        await Future.delayed(const Duration(milliseconds: 200));
      }
      await FlutterBluePlus.stopScan();

      if (found == null) {
        setState(() {
          _busy = false;
          _status = "Nicht gefunden. Ist Bluetooth an der Uhr aktiviert?";
        });
        return;
      }

      _device = found;
      setState(() => _status = "Verbinde ...");

      await found.connect(
        license: License.nonprofit,
        timeout: const Duration(seconds: 20),
      );
      final services = await found.discoverServices();

      _timeChar = null;
      _statsChar = null;
      _clearChar = null;
      _ttChar = null;
      _lessonChar = null;
      for (final s in services) {
        for (final c in s.characteristics) {
          if (c.uuid.str == charTimeUuid) {
            _timeChar = c;
          } else if (c.uuid.str == charStatsUuid) {
            _statsChar = c;
          } else if (c.uuid.str == charClearUuid) {
            _clearChar = c;
          } else if (c.uuid.str == charTtUuid) {
            _ttChar = c;
          } else if (c.uuid.str == charLessonUuid) {
            _lessonChar = c;
          }
        }
      }

      setState(() {
        _connected = true;
        _status = "Verbunden.";
      });

      await _syncTime();
      await _readStats();
    } catch (e) {
      setState(() => _status = "Fehler: $e");
    } finally {
      if (mounted) setState(() => _busy = false);
    }
  }

  Future<void> _syncTime() async {
    if (_timeChar == null) {
      _status = "Zeit-Charakteristik nicht gefunden.";
      return;
    }
    final now = DateTime.now();
    final bytes = Uint8List.fromList([
      now.year - 2000,
      now.month,
      now.day,
      now.hour,
      now.minute,
      now.second,
    ]);
    await _timeChar!.write(bytes, withoutResponse: false);
    if (mounted) setState(() => _status = "Zeit & Datum synchronisiert.");
  }

  Future<void> _readStats() async {
    if (_statsChar == null) {
      _status = "Statistik-Charakteristik nicht gefunden.";
      return;
    }
    final value = await _statsChar!.read();
    final text = utf8.decode(value, allowMalformed: true);

    Map<String, dynamic>? stats;
    try {
      stats = jsonDecode(text) as Map<String, dynamic>;
    } catch (_) {
      stats = null;
    }

    // Stunden-Statistik liegt in einer eigenen Charakteristik (BLE-Größenlimit).
    // Sie ist optional, damit die App auch mit älterer Firmware funktioniert.
    if (_lessonChar != null) {
      try {
        final lessonValue = await _lessonChar!.read();
        final lessonText = utf8.decode(lessonValue, allowMalformed: true);
        final lesson = jsonDecode(lessonText);
        if (lesson is Map<String, dynamic>) {
          (stats ??= {})['lessonCounts'] = lesson['lessonCounts'];
        }
      } catch (_) {
        // Stunden-Statistik konnte nicht gelesen werden – ignorieren.
      }
    }

    if (mounted) {
      setState(() {
        _stats = stats;
        if (stats == null) _status = "Ungültige Statistik-Daten empfangen.";
      });
    }
  }

  Future<void> _clearStats() async {
    if (_clearChar == null) {
      _status = "Lösch-Charakteristik nicht gefunden.";
      return;
    }
    await _clearChar!.write(utf8.encode("1"), withoutResponse: false);
    await Future.delayed(const Duration(milliseconds: 400));
    await _readStats();
    if (mounted) setState(() => _status = "Statistik auf der Uhr gelöscht.");
  }

  Future<void> _disconnect() async {
    if (_device != null) {
      try {
        await _device!.disconnect();
      } catch (_) {}
    }
    if (mounted) {
      setState(() {
        _connected = false;
        _stats = null;
        _status = "Getrennt.";
      });
    }
  }

  void _openTimetable() {
    Navigator.of(context).push(
      MaterialPageRoute(
        builder: (_) => TimetablePage(store: _ttStore, ttChar: _connected ? _ttChar : null),
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text("Meldezähler"),
        actions: [
          IconButton(
            icon: const Icon(Icons.calendar_month),
            tooltip: "Stundenplan",
            onPressed: _openTimetable,
          ),
          if (_connected)
            IconButton(
              icon: const Icon(Icons.link_off),
              tooltip: "Trennen",
              onPressed: _busy ? null : _disconnect,
            ),
        ],
      ),
      body: ListView(
        padding: const EdgeInsets.all(16),
        children: [
          Text(_status, style: Theme.of(context).textTheme.bodyLarge),
          const SizedBox(height: 12),
          ElevatedButton.icon(
            onPressed: _busy ? null : (_connected ? _disconnect : _connect),
            icon: Icon(_connected ? Icons.link_off : Icons.bluetooth_searching),
            label: Text(
              _busy
                  ? "Bitte warten ..."
                  : (_connected ? "Trennen" : "Verbinden & Zeit synchronisieren"),
            ),
          ),
          const SizedBox(height: 12),
          if (_connected) ...[
            Card(
              child: Padding(
                padding: const EdgeInsets.all(16),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text("Statistik", style: Theme.of(context).textTheme.titleLarge),
                    const SizedBox(height: 8),
                    _statRow("Meldungen heute", _stats?['total']),
                    _statRow("In Session", _stats?['session']),
                    _statRow("Seit Kalibrierung", _stats?['seitCalib']),
                    _statRow("Akku", _battText(_stats?['batt'])),
                    const SizedBox(height: 16),
                    Text("Verlauf pro Minute",
                        style: Theme.of(context).textTheme.bodyMedium),
                    const SizedBox(height: 8),
                    _barChart(_stats?['min']),
                    const SizedBox(height: 16),
                    Text("Stunden-Statistik",
                        style: Theme.of(context).textTheme.titleMedium),
                    const SizedBox(height: 8),
                    _lessonStats(_stats?['lessonCounts']),
                    const SizedBox(height: 16),
                    Row(
                      children: [
                        Expanded(
                          child: ElevatedButton.icon(
                            onPressed: _busy ? null : _syncTime,
                            icon: const Icon(Icons.schedule),
                            label: const Text("Zeit sync"),
                          ),
                        ),
                        const SizedBox(width: 8),
                        Expanded(
                          child: OutlinedButton.icon(
                            onPressed: _busy ? null : _readStats,
                            icon: const Icon(Icons.refresh),
                            label: const Text("Aktualisieren"),
                          ),
                        ),
                      ],
                    ),
                    const SizedBox(height: 8),
                    SizedBox(
                      width: double.infinity,
                      child: FilledButton.icon(
                        style: FilledButton.styleFrom(
                          backgroundColor: Colors.red.shade600,
                        ),
                        onPressed: _busy ? null : _clearStats,
                        icon: const Icon(Icons.delete_forever),
                        label: const Text("Statistik auf der Uhr löschen"),
                      ),
                    ),
                  ],
                ),
              ),
            ),
          ],
        ],
      ),
    );
  }

  Widget _statRow(String label, dynamic value) {
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 2),
      child: Row(
        mainAxisAlignment: MainAxisAlignment.spaceBetween,
        children: [
          Text(label),
          Text(value == null ? "–" : "$value",
              style: const TextStyle(fontWeight: FontWeight.bold)),
        ],
      ),
    );
  }

  String _battText(dynamic b) {
    if (b == null || b == -1) return "unbekannt";
    return "$b %";
  }

  Widget _barChart(dynamic min) {
    final list = (min is List) ? min.cast<num>() : <num>[];
    if (list.isEmpty) {
      return const SizedBox(height: 90, child: Center(child: Text("–")));
    }
    int maxV = 0;
    for (final e in list) {
      final v = e.toInt();
      if (v > maxV) maxV = v;
    }
    if (maxV == 0) maxV = 1;

    return SizedBox(
      height: 90,
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.end,
        children: List.generate(list.length, (i) {
          final h = (list[i].toInt() / maxV * 80).clamp(2.0, 80.0).toDouble();
          return Expanded(
            child: Container(
              margin: const EdgeInsets.symmetric(horizontal: 1),
              height: h,
              decoration: BoxDecoration(
                color: list[i].toInt() > 0 ? Colors.teal : Colors.grey.shade300,
                borderRadius: BorderRadius.circular(2),
              ),
            ),
          );
        }),
      ),
    );
  }

  Widget _lessonStats(dynamic lessonCounts) {
    if (lessonCounts is! List) {
      return Text("Kein Stundenplan aktiv.",
          style: Theme.of(context).textTheme.bodyMedium);
    }
    final rows = <Widget>[];
    bool any = false;
    for (int d = 0; d < 7; d++) {
      final periods = _ttStore.sortedPeriods(d);
      if (periods.isEmpty) continue;
      final counts = (d < lessonCounts.length) ? lessonCounts[d] : null;
      final children = <Widget>[];
      for (int i = 0; i < periods.length; i++) {
        final c = (counts is List && i < counts.length) ? counts[i] : 0;
        children.add(Padding(
          padding: const EdgeInsets.symmetric(vertical: 1),
          child: Row(
            mainAxisAlignment: MainAxisAlignment.spaceBetween,
            children: [
              Text("  ${periods[i].name} (${periods[i].startLabel})"),
              Text("$c", style: const TextStyle(fontWeight: FontWeight.bold)),
            ],
          ),
        ));
      }
      rows.add(Padding(
        padding: const EdgeInsets.only(top: 8, bottom: 2),
        child: Text(dayNames[d],
            style: const TextStyle(fontWeight: FontWeight.w600)),
      ));
      rows.addAll(children);
      any = true;
    }
    if (!any) {
      return Text("Kein Stundenplan aktiv.",
          style: Theme.of(context).textTheme.bodyMedium);
    }
    return Column(crossAxisAlignment: CrossAxisAlignment.start, children: rows);
  }
}
