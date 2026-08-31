#include "MainWindow.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QStyleFactory>
#include <QTextStream>

namespace {

void logMessage(QtMsgType type, const QMessageLogContext&, const QString& message) {
  const auto directory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/logs";
  QDir{}.mkpath(directory);
  QFile file(directory + "/atlas.log");
  if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) return;
  const char* level = type == QtDebugMsg ? "DEBUG" : type == QtInfoMsg ? "INFO" :
                      type == QtWarningMsg ? "WARN" : type == QtCriticalMsg ? "ERROR" : "FATAL";
  QTextStream(&file) << QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)
                     << " [" << level << "] " << message << '\n';
}

}  // namespace

int main(int argc, char* argv[]) {
  QApplication application(argc, argv);
  QApplication::setApplicationName("Morphis Atlas");
  QApplication::setOrganizationName("Morphis");
  QApplication::setApplicationVersion("0.1.0");
  qInstallMessageHandler(logMessage);
  application.setStyle(QStyleFactory::create("Fusion"));
  // Fusion falls back to the platform accent color for palette roles the stylesheet
  // below doesn't cover (e.g. alternating list/tree row backgrounds), which reads as
  // a jarring stray color on any system with a saturated Windows accent. Pin an
  // explicit dark palette so Atlas looks the same regardless of the user's accent.
  QPalette darkPalette;
  darkPalette.setColor(QPalette::Window, QColor(0x17, 0x19, 0x1d));
  darkPalette.setColor(QPalette::WindowText, QColor(0xe7, 0xe9, 0xed));
  darkPalette.setColor(QPalette::Base, QColor(0x11, 0x13, 0x18));
  darkPalette.setColor(QPalette::AlternateBase, QColor(0x1b, 0x1e, 0x24));
  darkPalette.setColor(QPalette::ToolTipBase, QColor(0x23, 0x27, 0x2e));
  darkPalette.setColor(QPalette::ToolTipText, QColor(0xe7, 0xe9, 0xed));
  darkPalette.setColor(QPalette::Text, QColor(0xe7, 0xe9, 0xed));
  darkPalette.setColor(QPalette::Button, QColor(0x28, 0x2d, 0x35));
  darkPalette.setColor(QPalette::ButtonText, QColor(0xe7, 0xe9, 0xed));
  darkPalette.setColor(QPalette::BrightText, Qt::red);
  darkPalette.setColor(QPalette::Highlight, QColor(0x31, 0x5f, 0x91));
  darkPalette.setColor(QPalette::HighlightedText, Qt::white);
  darkPalette.setColor(QPalette::Disabled, QPalette::Text, QColor(0x6b, 0x72, 0x7c));
  darkPalette.setColor(QPalette::Disabled, QPalette::WindowText, QColor(0x6b, 0x72, 0x7c));
  application.setPalette(darkPalette);
  application.setStyleSheet(R"css(
    QWidget { background: #17191d; color: #e7e9ed; font-size: 10pt; }
    QMainWindow::separator { background: #2b3038; width: 4px; height: 4px; }
    QLineEdit, QTreeView, QListView, QTreeWidget, QTableWidget {
      background: #111318; border: 1px solid #303640; border-radius: 3px;
    }
    QLineEdit { padding: 6px 9px; selection-background-color: #3e74ad; }
    QTreeView::item, QTreeWidget::item { min-height: 24px; padding: 2px; }
    QTreeView::item:selected, QListView::item:selected, QTreeWidget::item:selected {
      background: #315f91; color: white;
    }
    QToolBar { background: #1d2025; border: 0; spacing: 7px; padding: 7px; }
    QToolButton, QPushButton {
      background: #282d35; border: 1px solid #39414c; border-radius: 4px; padding: 6px 10px;
    }
    QToolButton:hover, QPushButton:hover { background: #343b46; border-color: #4c76a3; }
    QToolButton:pressed, QPushButton:pressed { background: #294e76; }
    QDockWidget::title { background: #23272e; padding: 8px; font-weight: 600; }
    QTabBar::tab { background: #23272e; padding: 9px 20px; border-right: 1px solid #15171b; }
    QTabBar::tab:selected { background: #315f91; color: white; }
    QHeaderView::section { background: #252a31; border: 0; border-right: 1px solid #343a44; padding: 7px; }
    QLabel#PanelHint { color: #89919d; font-size: 9pt; }
    QStatusBar { color: #aab1bc; background: #1d2025; }
  )css");
  atlas::MainWindow window;
  window.show();
  return application.exec();
}
