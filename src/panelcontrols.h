#pragma once
//
// The sliding panel's controls, the PicoScope way, applied once to the built
// pages so every control keeps its own code and signals:
//  - exclusive button groups in a row: joined segments; in a grid: flat
//    buttons, the selected one filled;
//  - a single check box: its text as a label over an Off | On pair (the box
//    stays, hidden, as the setting); check boxes in a grid: toggle buttons;
//  - dropdowns and number fields: compact-card fields, - and + bars round
//    the value, 65 % of the width; clicking a number opens a keypad.
//
class QAbstractSpinBox;
class QObject;
class QWidget;

namespace PanelControls {

// root: the panel's pages; groupOwner: where the QButtonGroups live.
void adopt(QWidget *root, QObject *groupOwner);

// Colours that aren't in style sheets (the bars' icons): on theme changes.
void restyle(QWidget *root);

// The keypad for a number field (also what clicking the field opens).
void openNumericPad(QAbstractSpinBox *spin);

} // namespace PanelControls
