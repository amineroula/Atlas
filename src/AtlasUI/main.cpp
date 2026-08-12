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
  application.setStyleSheet(R"css(
    QWidget { background: #17191d; color: #e7e9ed; font-size: 10pt; }
    QLineEdit, QTreeView, QListView, QTreeWidget { background: #111318; border: 1px solid #30343c; }
    QTreeView::item:selected, QListView::item:selected { background: #335d92; }
    QToolBar { border: 0; spacing: 6px; padding: 6px; }
    QDockWidget::title { background: #22262c; padding: 6px; }
    QTabBar::tab { background: #22262c; padding: 8px 18px; }
    QTabBar::tab:selected { background: #335d92; }
    QStatusBar { color: #9da3ae; }
  )css");
  atlas::MainWindow window;
  window.show();
  return application.exec();
}
