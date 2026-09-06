#include <AtlasCore/AssetCatalogModel.h>
#include <algorithm>
#include <iterator>
#include <utility>
namespace atlas {
void AssetCatalogModel::upsert(CatalogAsset asset) { const auto same=[&](const CatalogAsset& a){return (!asset.atlasId.empty()&&a.atlasId==asset.atlasId)||(!asset.source.provider.empty()&&!asset.source.vendorId.empty()&&a.source.provider==asset.source.provider&&a.source.vendorId==asset.source.vendorId);}; auto it=std::find_if(assets_.begin(),assets_.end(),same); if(it==assets_.end()) assets_.push_back(std::move(asset)); else *it=std::move(asset); }
void AssetCatalogModel::upsertFolder(LibraryFolder folder){auto it=std::find_if(folders_.begin(),folders_.end(),[&](const LibraryFolder& f){return f.id==folder.id;}); if(it==folders_.end()) folders_.push_back(std::move(folder)); else *it=std::move(folder);}
bool AssetCatalogModel::addAssetToFolder(const std::string& assetId,const std::string& folderId){auto f=std::find_if(folders_.begin(),folders_.end(),[&](const LibraryFolder& v){return v.id==folderId;}); if(f==folders_.end()) return false; if(std::find(f->assetIds.begin(),f->assetIds.end(),assetId)==f->assetIds.end()) f->assetIds.push_back(assetId); return true;}
bool AssetCatalogModel::removeAssetFromFolder(const std::string& assetId,const std::string& folderId){auto f=std::find_if(folders_.begin(),folders_.end(),[&](const LibraryFolder& v){return v.id==folderId;}); if(f==folders_.end()) return false; f->assetIds.erase(std::remove(f->assetIds.begin(),f->assetIds.end(),assetId),f->assetIds.end()); return true;}
bool AssetCatalogModel::setRating(const std::string& assetId,int rating){if(rating<0||rating>5)return false; auto a=std::find_if(assets_.begin(),assets_.end(),[&](const CatalogAsset& v){return v.atlasId==assetId;}); if(a==assets_.end())return false; a->rating=rating; return true;}
const std::vector<CatalogAsset>& AssetCatalogModel::assets()const noexcept{return assets_;} const std::vector<LibraryFolder>& AssetCatalogModel::folders()const noexcept{return folders_;}
std::vector<CatalogAsset> AssetCatalogModel::byKind(AssetKind kind)const{std::vector<CatalogAsset> r;std::copy_if(assets_.begin(),assets_.end(),std::back_inserter(r),[&](const CatalogAsset&a){return a.kind==kind;});return r;}
std::vector<CatalogAsset> AssetCatalogModel::byCategory(const std::string& v)const{std::vector<CatalogAsset> r;std::copy_if(assets_.begin(),assets_.end(),std::back_inserter(r),[&](const CatalogAsset&a){return a.category==v;});return r;}
std::vector<CatalogAsset> AssetCatalogModel::byProvider(const std::string& v)const{std::vector<CatalogAsset> r;std::copy_if(assets_.begin(),assets_.end(),std::back_inserter(r),[&](const CatalogAsset&a){return a.source.provider==v;});return r;}
std::vector<CatalogAsset> AssetCatalogModel::byAvailability(AssetAvailability v)const{std::vector<CatalogAsset> r;std::copy_if(assets_.begin(),assets_.end(),std::back_inserter(r),[&](const CatalogAsset&a){return a.availability==v;});return r;}
} // namespace atlas
