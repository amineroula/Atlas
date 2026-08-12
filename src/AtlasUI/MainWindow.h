#pragma once

#include <AtlasJobs/JobQueue.h>

#include <QMainWindow>
#include <QString>

#include <filesystem>
#include <memory>

class QAction;
class QFileSystemModel;
class QLabel;
class QLineEdit;
class QListWidget;
class QTabWidget;
class QTreeWidget;
class QTreeView;

namespace atlas {

class BrowserTab;
class Catalog;

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
  void showStatistics();
  void showGlobalSearch();
  void renameSelection();
  void duplicateSelection();
  void transferSelection(bool move);
  void trashSelection();
  void createFolder();
  void revealProperties();
  void archiveSelection();
  void extractSelection();
  void runArchiveCommand(const QStringList& arguments, const QString& description);
  std::filesystem::path catalogPath() const;

  QFileSystemModel* model_{};
  QTabWidget* tabs_{};
  QLineEdit* location_{};
  QLineEdit* search_{};
  QListWidget* bookmarks_{};
  QTreeWidget* jobsView_{};
  QTreeView* folderTree_{};
  QLabel* preview_{};
  QLabel* metadata_{};
  QAction* backAction_{};
  QAction* forwardAction_{};
  QAction* upAction_{};
  QAction* dualPaneAction_{};
  QAction* detailsAction_{};
  std::unique_ptr<Catalog> catalog_;
  JobQueue jobs_;
  JobId previewJob_{};
  QString previewPath_;
};

}  // namespace atlas
