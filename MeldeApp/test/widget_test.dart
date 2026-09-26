// Widget-Test für die Meldezähler-App.
//
// Der Test prüft, dass die App startet und die Startansicht (HomePage)
// mit dem Verbinden-Button angezeigt wird.

import 'package:flutter_test/flutter_test.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'package:melde_app/main.dart';

void main() {
  testWidgets('App startet und zeigt den Verbinden-Button',
      (WidgetTester tester) async {
    // SharedPreferences in der Test-Umgebung mocken, damit der
    // Stundenplan-Store beim Start nicht auf das echte Plugin zugreift.
    SharedPreferences.setMockInitialValues({});

    await tester.pumpWidget(const MeldeApp());
    await tester.pumpAndSettle();

    // Titel der AppBar
    expect(find.text('Meldezähler'), findsOneWidget);

    // Start-Status und Verbinden-Button
    expect(find.text('Bereit'), findsOneWidget);
    expect(find.text('Verbinden & Zeit synchronisieren'), findsOneWidget);
  });
}
