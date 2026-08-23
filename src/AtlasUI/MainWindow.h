#pragma once

#include <AtlasJobs/JobQueue.h>

#include <QMainWindow>
#include <QString>
#include <QHash>

#include <filesystem>
#include <memory>

class QAction;
class QDockWidget;
class QFileSystemModel;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QProgressBar;
class QSlider;
class QTabWidget;
class QTreeWidget;
class QTreeView;

namespace atlas {

class BrowserTab;
class Catalog;
struct StorageAnalysis;

class MainWindow final : public QMainWindow {
 public:
  explicit MainWindow(QWidget* parent = nullptr);
  ~MainWindow() override;

 protected:
  void closeEvent(QCloseEvent* event) override;

 private:
  BrowserTab* currentTab() const;
  void createActions();
  void createMenusAndToolbars();
  void createDocks();
  void addTab(const QString& path);
  void restoreSession();
  void saveSession();
  void updateNavigationState();
  void updatePreview(const QString& path);
  void refreshBookmarks();
  void addCurrentBookmark();
  void indexCurrentFolder();
  void startPersistentScan(const QString& path, bool driveScan);
  void runPersistentScan(std::int64_t sessionId);
  void refreshScanSessions();
  void pauseSelectedScan();
  void resumeSelectedScan();
  void saveSelectedScanAs();
  void showSelectedScanResults();
  void showOrganizedScan(std::int64_t sessionId);
  void showStatistics();
  void showDriveStatistics();
  void analyzePath(const QString& path, const QString& title, bool findDuplicates);
  void showAnalysisResults(const std::filesystem::path& root, const QString& title,
                           bool findDuplicates, StorageAnalysis analysis);
  void showGlobalSearch();
  void renameSelection();
  void duplicateSelection();
  void transferSelection(bool move);
  void enqueueTransfer(const QStringList& paths, const QString& destination,
                       bool move, bool renameConflicts);
  void rememberSelection(bool move);
  void pasteHere();
  void trashSelection();
  void createFolder();
  void revealProperties();
  void archiveSelection();
  void extractSelection();
  void runArchiveCommand(const QStringList& arguments, const QString& description);
  void applyDefaultLayout();
  void chooseCacheFolder();
  void resetCacheFolder();
  void applyCacheFolder(const QString& path);
  [[nodiscard]] QString systemCacheFolder() const;
  std::filesystem::path catalogPath() const;
  void chooseCatalogFolder();
  void resetCatalogFolder();
  void moveCatalogTo(const QString& directory);
  [[nodiscard]] std::filesystem::path defaultCatalogDirectory() const;

  QFileSystemModel* model_{};
  QTabWidget* tabs_{};
  QLineEdit* location_{};
  QLineEdit* search_{};
  QListWidget* bookmarks_{};
  QTreeWidget* jobsView_{};
  QTreeWidget* scansView_{};
  QPlainTextEdit* scannerActivity_{};
  QProgressBar* activityProgress_{};
  QLabel* activityTitle_{};
  QTreeView* folderTree_{};
  QDockWidget* foldersDock_{};
  QDockWidget* bookmarksDock_{};
  QDockWidget* previewDock_{};
  QDockWidget* jobsDock_{};
  QDockWidget* scansDock_{};
  QLabel* preview_{};
  QLabel* metadata_{};
  QAction* backAction_{};
  QAction* forwardAction_{};
  QAction* upAction_{};
  QAction* dualPaneAction_{};
  QAction* detailsAction_{};
  QAction* pasteAction_{};
  QSlider* thumbnailSize_{};
  std::unique_ptr<Catalog> catalog_;
  JobQueue jobs_;
  JobId previewJob_{};
  QString previewPath_;
  QStringList transferClipboard_;
  bool transferClipboardMoves_{};
  QHash<JobId, QString> lastJobDetails_;
  QHash<std::int64_t, JobId> scanJobs_;
  QString cacheRoot_;
};

}  // namespace atlas
