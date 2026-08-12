#include <AtlasDatabase/Catalog.h>
#include <AtlasCore/FileOperations.h>
#include <AtlasCore/StorageAnalyzer.h>
#include <AtlasJobs/JobQueue.h>
#include <AtlasScanner/Scanner.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <atomic>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace {

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    path_ = std::filesystem::temp_directory_path() /
            ("atlas-test-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(path_);
  }
  ~TemporaryDirectory() { std::filesystem::remove_all(path_); }
  const std::filesystem::path& path() const { return path_; }
 private:
  std::filesystem::path path_;
};

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void catalogPreservesIdentity() {
  TemporaryDirectory temporary;
  atlas::Catalog catalog(temporary.path() / "catalog.db");
  atlas::AssetMetadata metadata{
      .path = temporary.path() / "image.exr",
      .sizeBytes = 512,
      .modifiedAt = std::chrono::system_clock::now(),
      .isDirectory = false};
  const auto firstId = catalog.upsert(metadata);
  metadata.sizeBytes = 1024;
  const auto secondId = catalog.upsert(metadata);
  require(firstId == secondId, "upsert changed stable asset ID");
  require(catalog.assetCount() == 1, "upsert duplicated an asset");
  const auto stored = catalog.findByPath(metadata.path);
  require(stored.has_value(), "stored asset was not found");
  require(stored->metadata.sizeBytes == 1024, "stored metadata was not refreshed");
}

void catalogSearchesAndMaintainsState() {
  TemporaryDirectory temporary;
  atlas::Catalog catalog(temporary.path() / "catalog.db");
  const auto now = std::chrono::system_clock::now();
  const std::vector<atlas::AssetMetadata> assets{
      {.path = temporary.path() / "textures", .modifiedAt = now, .isDirectory = true},
      {.path = temporary.path() / "textures" / "hero_albedo.png", .sizeBytes = 4096, .modifiedAt = now},
      {.path = temporary.path() / "textures" / "hero_normal.exr", .sizeBytes = 8192, .modifiedAt = now},
      {.path = temporary.path() / "notes.txt", .sizeBytes = 32, .modifiedAt = now}};
  const auto ids = catalog.upsertBatch(assets);
  require(ids.size() == assets.size(), "batch upsert did not return all IDs");
  atlas::AssetQuery query;
  query.text = "hero";
  query.extension = "png";
  query.beneath = temporary.path();
  const auto matches = catalog.search(query);
  require(matches.size() == 1, "catalog filters returned the wrong results");
  require(matches.front().metadata.sizeBytes == 4096, "catalog search returned wrong asset");
  const auto statistics = catalog.statistics();
  require(statistics.files == 3 && statistics.directories == 1 && statistics.bytes == 12320,
          "catalog statistics are incorrect");
  require(catalog.largestFiles(1).front().metadata.sizeBytes == 8192,
          "largest file query is incorrect");
  catalog.addBookmark(temporary.path() / "textures");
  require(catalog.bookmarks().size() == 1, "bookmark was not stored");
  catalog.removeBookmark(temporary.path() / "textures");
  require(catalog.bookmarks().empty(), "bookmark was not removed");
  const std::vector<std::filesystem::path> observed{assets[0].path, assets[1].path, assets[3].path};
  require(catalog.removeMissingUnder(temporary.path(), observed) == 1,
          "stale catalog entry was not removed");
}

void scannerReportsContents() {
  TemporaryDirectory temporary;
  std::filesystem::create_directory(temporary.path() / "textures");
  std::ofstream(temporary.path() / "textures" / "albedo.png") << "pixels";
  std::ofstream(temporary.path() / "notes.txt") << "atlas";
  std::uint64_t callbacks{};
  const auto summary = atlas::Scanner{}.scan(
      temporary.path(), std::stop_token{},
      [&callbacks](const atlas::AssetMetadata&) { ++callbacks; });
  require(summary.files == 2, "scanner file count is incorrect");
  require(summary.directories == 1, "scanner directory count is incorrect");
  require(summary.bytes == 11, "scanner byte count is incorrect");
  require(callbacks == 3, "scanner did not report every entry");
  require(!summary.cancelled, "scanner unexpectedly cancelled");
}

void scannerHonorsCancellation() {
  TemporaryDirectory temporary;
  std::ofstream(temporary.path() / "file.txt") << "content";
  std::stop_source source;
  source.request_stop();
  const auto summary = atlas::Scanner{}.scan(
      temporary.path(), source.get_token(), [](const atlas::AssetMetadata&) {});
  require(summary.cancelled, "scanner ignored cancellation");
  require(summary.files == 0, "cancelled scanner processed files");
}

void scannerHonorsDepth() {
  TemporaryDirectory temporary;
  std::filesystem::create_directories(temporary.path() / "one" / "two");
  std::ofstream(temporary.path() / "one" / "top.txt") << "top";
  std::ofstream(temporary.path() / "one" / "two" / "deep.txt") << "deep";
  atlas::ScanOptions options;
  options.maximumDepth = 0;
  const auto summary = atlas::Scanner{}.scan(temporary.path(), {}, [](const auto&) {}, options);
  require(summary.directories == 1, "depth-limited scanner missed first level");
  require(summary.files == 0, "depth-limited scanner recursed too far");
}

void fileOperationsAreConflictAware() {
  TemporaryDirectory temporary;
  const auto sourceDirectory = temporary.path() / "source";
  const auto targetDirectory = temporary.path() / "target";
  std::filesystem::create_directories(sourceDirectory);
  std::filesystem::create_directories(targetDirectory);
  const auto source = sourceDirectory / "asset.txt";
  std::ofstream(source) << "atlas";
  auto result = atlas::FileOperations::transfer(atlas::FileOperationKind::Copy, {source},
      targetDirectory, atlas::ConflictPolicy::Skip);
  require(result.succeeded == 1 && std::filesystem::exists(targetDirectory / "asset.txt"),
          "copy operation failed");
  result = atlas::FileOperations::transfer(atlas::FileOperationKind::Copy, {source},
      targetDirectory, atlas::ConflictPolicy::Rename);
  require(result.succeeded == 1 && std::filesystem::exists(targetDirectory / "asset (2).txt"),
          "rename-on-conflict failed");
  const auto duplicate = atlas::FileOperations::duplicate(source);
  require(std::filesystem::exists(duplicate), "duplicate operation failed");
  const auto renamed = sourceDirectory / "renamed.txt";
  atlas::FileOperations::rename(source, renamed);
  require(std::filesystem::exists(renamed), "rename operation failed");
}

void jobQueueRunsAndCancelsWork() {
  atlas::JobQueue queue(2);
  std::atomic<int> completed{};
  const auto first = queue.submit("complete", [&completed](std::stop_token, const atlas::JobReporter& report) {
    report(0.5, "half");
    ++completed;
  });
  const auto second = queue.submit("cancel", [](std::stop_token token, const atlas::JobReporter&) {
    while (!token.stop_requested()) std::this_thread::yield();
  });
  require(queue.cancel(second), "running or queued job could not be cancelled");
  require(queue.waitForIdle(std::chrono::seconds(5)), "job queue did not become idle");
  require(completed == 1, "job queue did not execute work");
  const auto snapshots = queue.snapshots();
  require(snapshots.at(static_cast<std::size_t>(first - 1)).state == atlas::JobState::Completed,
          "completed job has wrong state");
  require(snapshots.at(static_cast<std::size_t>(second - 1)).state == atlas::JobState::Cancelled,
          "cancelled job has wrong state");
}

void assetKindsAreRecognized() {
  require(atlas::classifyAsset("texture.EXR") == atlas::AssetKind::Image,
          "image classification is not case insensitive");
  require(atlas::classifyAsset("mesh.fbx") == atlas::AssetKind::Model,
          "model classification failed");
  require(atlas::classifyAsset("folder", true) == atlas::AssetKind::Directory,
          "directory classification failed");
}

void storageAnalysisFindsWaste() {
  TemporaryDirectory temporary;
  std::filesystem::create_directories(temporary.path() / "empty");
  std::filesystem::create_directories(temporary.path() / "assets");
  std::ofstream(temporary.path() / "assets" / "first.bin") << "duplicate content";
  std::ofstream(temporary.path() / "assets" / "second.bin") << "duplicate content";
  std::ofstream(temporary.path() / "assets" / "unique.bin") << "unique";
  const auto analysis = atlas::StorageAnalyzer{}.analyze(temporary.path());
  require(analysis.files == 3, "storage analyzer file count is incorrect");
  require(!analysis.emptyFolders.empty(), "storage analyzer missed an empty folder");
  require(analysis.duplicates.size() == 1 && analysis.duplicates.front().paths.size() == 2,
          "storage analyzer missed exact duplicates");
  require(!analysis.largestFiles.empty() && !analysis.largestFolders.empty(),
          "storage analyzer did not rank paths");
}

}  // namespace

int main() {
  try {
    catalogPreservesIdentity();
    catalogSearchesAndMaintainsState();
    scannerReportsContents();
    scannerHonorsCancellation();
    scannerHonorsDepth();
    fileOperationsAreConflictAware();
    jobQueueRunsAndCancelsWork();
    assetKindsAreRecognized();
    storageAnalysisFindsWaste();
    std::cout << "All Atlas foundation tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Test failure: " << error.what() << '\n';
    return 1;
  }
}
