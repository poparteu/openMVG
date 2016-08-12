#include <openMVG/localization/VoctreeLocalizer.hpp>
#if HAVE_CCTAG
#include <openMVG/localization/CCTagLocalizer.hpp>
#endif
#include <openMVG/rig/Rig.hpp>
#include <openMVG/sfm/pipelines/localization/SfM_Localizer.hpp>
#include <openMVG/image/image_io.hpp>
#include <openMVG/dataio/FeedProvider.hpp>
#include <openMVG/logger.hpp>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/progress.hpp>
#include <boost/program_options.hpp> 
#include <boost/accumulators/accumulators.hpp>
#include <boost/accumulators/statistics/stats.hpp>
#include <boost/accumulators/statistics/mean.hpp>
#include <boost/accumulators/statistics/min.hpp>
#include <boost/accumulators/statistics/max.hpp>
#include <boost/accumulators/statistics/sum.hpp>

#include <iostream>
#include <string>
#include <chrono>

#if HAVE_ALEMBIC
#include <openMVG/sfm/AlembicExporter.hpp>
#endif // HAVE_ALEMBIC


namespace bfs = boost::filesystem;
namespace bacc = boost::accumulators;
namespace po = boost::program_options;

using namespace openMVG;

enum DescriberType
{
  SIFT
#if HAVE_CCTAG
  ,CCTAG,
  SIFT_CCTAG
#endif
};

inline DescriberType stringToDescriberType(const std::string& describerType)
{
  if(describerType == "SIFT")
    return DescriberType::SIFT;
#if HAVE_CCTAG
  if (describerType == "CCTAG")
    return DescriberType::CCTAG;
  if(describerType == "SIFT_CCTAG")
    return DescriberType::SIFT_CCTAG;
#endif
  throw std::invalid_argument("Unsupported describer type "+describerType);
}

inline std::string describerTypeToString(DescriberType describerType)
{
  if(describerType == DescriberType::SIFT)
    return "SIFT";
#if HAVE_CCTAG
  if (describerType == DescriberType::CCTAG)
    return "CCTAG";
  if(describerType == DescriberType::SIFT_CCTAG)
    return "SIFT_CCTAG";
#endif
  throw std::invalid_argument("Unrecognized DescriberType "+std::to_string(describerType));
}

std::ostream& operator<<(std::ostream& os, const DescriberType describerType)
{
  os << describerTypeToString(describerType);
  return os;
}

std::istream& operator>>(std::istream &in, DescriberType &describerType)
{
  int i;
  in >> i;
  describerType = static_cast<DescriberType>(i);
  return in;
}

std::string myToString(std::size_t i, std::size_t zeroPadding)
{
  std::stringstream ss;
  ss << std::setw(zeroPadding) << std::setfill('0') << i;
  return ss.str();
}

int main(int argc, char** argv)
{
  // common parameters
  std::string sfmFilePath;                //< the OpenMVG .json data file
  std::string descriptorsFolder;          //< the the folder containing the descriptors
  std::string mediaPath;                  //< the media file to localize
  std::string filelist;                   //< the media file to localize
  std::string outputFile;                 //< the name of the file where to store the calibration data
  std::string preset = features::describerPreset_enumToString(features::EDESCRIBER_PRESET::NORMAL_PRESET);               //< the preset for the feature extractor
  std::string str_descriptorType = describerTypeToString(DescriberType::SIFT);               //< the preset for the feature extractor
  bool refineIntrinsics = false;
  // parameters for voctree localizer
  std::string vocTreeFilepath;            //< the vocabulary tree file
  std::string weightsFilepath;            //< the vocabulary tree weights file
  std::string algostring = "FirstBest";   //< the localization algorithm to use for the voctree localizer
  size_t numResults = 10;                 //< number of documents to search when querying the voctree
  // parameters for cctag localizer
  size_t nNearestKeyFrames = 5;           //

#if HAVE_ALEMBIC
  std::string exportFile = "trackedcameras.abc"; //!< the export file
#endif
  
  std::size_t nCam = 3;
  po::options_description desc("This program is used to calibrate a camera rig composed of internally calibrated cameras");
  desc.add_options()
          ("help,h", "Print this message")
          ("sfmdata,d", po::value<std::string>(&sfmFilePath)->required(), "The sfm_data.json kind of file generated by OpenMVG [it could be also a bundle.out to use an older version of OpenMVG]")
          ("descriptorPath", po::value<std::string>(&descriptorsFolder), "Folder containing the .desc. If not provided, it will be assumed to be parent(sfmdata)/matches [for the older version of openMVG it is the list.txt]")
          ("mediapath,m", po::value<std::string>(&mediaPath)->required(), "The folder path containing all the synchronised image subfolders assocated to each camera")
          ("filelist", po::value<std::string>(&filelist), "An optional txt file containing the images to use for calibration. This file must have the same name in each camera folder and contains the list of images to load.")
          ("refineIntrinsics", po::bool_switch(&refineIntrinsics), "Enable/Disable camera intrinsics refinement for each localized image")
          ("nCameras", po::value<size_t>(&nCam)->default_value(nCam), "Number of cameras composing the rig")
          ("preset", po::value<std::string>(&preset)->default_value(preset), "Preset for the feature extractor when localizing a new image {LOW,NORMAL,HIGH,ULTRA}")
          ("outfile,o", po::value<std::string>(&outputFile)->required(), "The name of the file where to store the calibration data")
          ("descriptors", po::value<std::string>(&str_descriptorType)->default_value(str_descriptorType), "Type of descriptors to use (SIFT, CCTAG, SIFT_CCTAG)")
  // parameters for voctree localizer
          ("voctree,t", po::value<std::string>(&vocTreeFilepath), "[voctree] Filename for the vocabulary tree")
          ("weights,w", po::value<std::string>(&weightsFilepath), "[voctree] Filename for the vocabulary tree weights")
          ("algorithm", po::value<std::string>(&algostring)->default_value(algostring), "[voctree] Algorithm type: FirstBest=0, BestResult=1, AllResults=2, Cluster=3" )
          ("results,r", po::value<size_t>(&numResults)->default_value(numResults), "[voctree] Number of images to retrieve in database")
#if HAVE_CCTAG
  // parameters for cctag localizer
          ("nNearestKeyFrames", po::value<size_t>(&nNearestKeyFrames)->default_value(nNearestKeyFrames), "[CCTAG] Number of images to retrieve in database")
#endif
#if HAVE_ALEMBIC
          ("export,e", po::value<std::string>(&exportFile)->default_value(exportFile), "If Alambic is enabled, filename for the file containing the camera poses. Default : trackedcameras.abc")
#endif
          ;

  po::variables_map vm;

  try
  {
    po::store(po::parse_command_line(argc, argv, desc), vm);

    if(vm.count("help") || (argc == 1))
    {
      POPART_COUT(desc);
      return EXIT_SUCCESS;
    }

    po::notify(vm);
  }
  catch(boost::program_options::required_option& e)
  {
    POPART_CERR("ERROR: " << e.what() << std::endl);
    POPART_COUT("Usage:\n\n" << desc);
    return EXIT_FAILURE;
  }
  catch(boost::program_options::error& e)
  {
    POPART_CERR("ERROR: " << e.what() << std::endl);
    POPART_COUT("Usage:\n\n" << desc);
    return EXIT_FAILURE;
  }
  // just debugging prints
  {
    POPART_COUT("Program called with the following parameters:");
    POPART_COUT("\tsfmdata: " << sfmFilePath);
    POPART_COUT("\tmediapath: " << mediaPath);
    POPART_COUT("\tdescriptorPath: " << descriptorsFolder);
    if(!filelist.empty())
      POPART_COUT("\tfilelist: " << filelist);
    POPART_COUT("\trefineIntrinsics: " << refineIntrinsics);
    POPART_COUT("\tnCameras: " << nCam);
    POPART_COUT("\tpreset: " << preset);
    POPART_COUT("\tdescriptors: " << str_descriptorType);
    if((DescriberType::SIFT==stringToDescriberType(str_descriptorType))
#if HAVE_CCTAG
            ||(DescriberType::SIFT_CCTAG==stringToDescriberType(str_descriptorType))
#endif
      )
    {
      // parameters for voctree localizer
      POPART_COUT("\tvoctree: " << vocTreeFilepath);
      POPART_COUT("\tweights: " << weightsFilepath);
      POPART_COUT("\toutfile: " << outputFile);
      POPART_COUT("\talgorithm: " << algostring);
      POPART_COUT("\tresults: " << numResults);
    }
#if HAVE_CCTAG
    else
    {
      POPART_COUT("\tnNearestKeyFrames: " << nNearestKeyFrames);
    }
#endif

  }

  localization::LocalizerParameters *param;
  
  localization::ILocalizer *localizer;
  
  DescriberType describer = stringToDescriberType(str_descriptorType);
  
  // initialize the localizer according to the chosen type of describer
  if((DescriberType::SIFT==describer)
#if HAVE_CCTAG
            ||(DescriberType::SIFT_CCTAG==describer)
#endif
      )
  {
    POPART_COUT("Calibrating sequence using the voctree localizer");
    localizer = new localization::VoctreeLocalizer(sfmFilePath,
                                           descriptorsFolder,
                                           vocTreeFilepath,
                                           weightsFilepath
#if HAVE_CCTAG
                                           , DescriberType::SIFT_CCTAG==describer
#endif
                                           );
    param = new localization::VoctreeLocalizer::Parameters();
    param->_featurePreset = features::describerPreset_stringToEnum(preset);
    param->_refineIntrinsics = refineIntrinsics;
    localization::VoctreeLocalizer::Parameters *casted = static_cast<localization::VoctreeLocalizer::Parameters *>(param);
    casted->_algorithm = localization::VoctreeLocalizer::initFromString(algostring);;
    casted->_numResults = numResults;
    casted->_ccTagUseCuda = false;
  }
#if HAVE_CCTAG
  else
  {
    POPART_COUT("Calibrating sequence using the cctag localizer");
    localizer = new localization::CCTagLocalizer(sfmFilePath, descriptorsFolder);
    param = new localization::CCTagLocalizer::Parameters();
    param->_featurePreset = features::describerPreset_stringToEnum(preset);
    param->_refineIntrinsics = refineIntrinsics;
    localization::CCTagLocalizer::Parameters *casted = static_cast<localization::CCTagLocalizer::Parameters *>(param);
    casted->_nNearestKeyFrames = nNearestKeyFrames;
  }
#endif 


  if(!localizer->isInit())
  {
    POPART_CERR("ERROR while initializing the localizer!");
    return EXIT_FAILURE;
  }

#if HAVE_ALEMBIC
  dataio::AlembicExporter exporter(exportFile);
  exporter.addPoints(localizer->getSfMData().GetLandmarks());
#endif

  // Create a camera rig
  rig::Rig rig;

  // Loop over all cameras of the rig
  for(std::size_t idCamera = 0; idCamera < nCam; ++idCamera)
  {
    POPART_COUT("~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~");
    POPART_COUT("CAMERA " << idCamera);
    POPART_COUT("~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~\n\n");
    const std::string subMediaFilepath = mediaPath + "/" + std::to_string(idCamera);
    const std::string calibFile = subMediaFilepath + "/intrinsics.txt";
    const std::string feedPath = subMediaFilepath + "/"+filelist;

    // create the feedProvider
    dataio::FeedProvider feed(feedPath, calibFile);
    if(!feed.isInit())
    {
      POPART_CERR("ERROR while initializing the FeedProvider!");
      return EXIT_FAILURE;
    }

    //std::string featureFile, cameraResultFile, pointsFile;
    //featureFile = subMediaFilepath + "/cctag" + std::to_string(nRings) + "CC.out";
    //cameraResultFile = inputFolder + "/" + std::to_string(i) + "/cameras.txt";
    //std::ofstream result;
    //result.open(cameraResultFile);
    //pointsFile = inputFolder + "/points.txt";

    image::Image<unsigned char> imageGrey;
    cameras::Pinhole_Intrinsic_Radial_K3 queryIntrinsics;
    bool hasIntrinsics = false;

    size_t frameCounter = 0;
    std::string currentImgName;

    // Define an accumulator set for computing the mean and the
    // standard deviation of the time taken for localization
    bacc::accumulator_set<double, bacc::stats<bacc::tag::mean, bacc::tag::min, bacc::tag::max, bacc::tag::sum > > stats;

    // used to collect the match data result
    std::vector<localization::LocalizationResult> vLocalizationResults;
    while(feed.readImage(imageGrey, queryIntrinsics, currentImgName, hasIntrinsics))
    {
      POPART_COUT("******************************");
      POPART_COUT("FRAME " << myToString(frameCounter, 4));
      POPART_COUT("******************************");
      auto detect_start = std::chrono::steady_clock::now();
      localization::LocalizationResult localizationResult;
      const bool ok = localizer->localize(imageGrey,
                                          param,
                                          hasIntrinsics/*useInputIntrinsics*/,
                                          queryIntrinsics,
                                          localizationResult);
      assert( ok == localizationResult.isValid() );
      vLocalizationResults.emplace_back(localizationResult);
      auto detect_end = std::chrono::steady_clock::now();
      auto detect_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(detect_end - detect_start);
      POPART_COUT("\nLocalization took  " << detect_elapsed.count() << " [ms]");
      stats(detect_elapsed.count());
      
#if HAVE_ALEMBIC
      if(localizationResult.isValid())
      {
        exporter.appendCamera("camera"+std::to_string(idCamera)+"."+myToString(frameCounter,4), localizationResult.getPose(), &queryIntrinsics, subMediaFilepath, frameCounter, frameCounter);
      }
      else
      {
        // @fixme for now just add a fake camera so that it still can be see in MAYA
        exporter.appendCamera("camera"+std::to_string(idCamera)+".V."+myToString(frameCounter,4), geometry::Pose3(), &queryIntrinsics, subMediaFilepath, frameCounter, frameCounter);
      }
#endif
      ++frameCounter;
      feed.goToNextFrame();
    }

    rig.setTrackingResult(vLocalizationResults, idCamera);

    // print out some time stats
    POPART_COUT("\n\n******************************");
    POPART_COUT("Processed " << frameCounter << " images for camera " << idCamera);
    POPART_COUT("Processing took " << bacc::sum(stats) / 1000 << " [s] overall");
    POPART_COUT("Mean time for localization:   " << bacc::mean(stats) << " [ms]");
    POPART_COUT("Max time for localization:   " << bacc::max(stats) << " [ms]");
    POPART_COUT("Min time for localization:   " << bacc::min(stats) << " [ms]");
  }
  
  {
    // just for statistics purposes
    const std::size_t numRigCam = rig.nCams();
    POPART_COUT("\n\n******************************");
    for(std::size_t idCam = 0; idCam < numRigCam; ++idCam)
    {
      auto & currResult = rig.getLocalizationResults(idCam);
      std::size_t numLocalized = 0;
      for(const auto &curr : currResult)
      {
        if(curr.isValid())
          ++numLocalized;
      }
      POPART_COUT("Camera " << idCam << " localized " 
              << numLocalized << "/" << currResult.size());
    }
    
  }
  
  POPART_COUT("Rig calibration initialization...");
  if(!rig.initializeCalibration())
  {
    POPART_CERR("Unable to find a proper initialization for the relative poses! Aborting...");
    return EXIT_FAILURE;
  }
  POPART_COUT("Rig calibration optimization...");
  if(!rig.optimizeCalibration())
  {
    POPART_CERR("Unable to optimize the relative poses! Aborting...");
    return EXIT_FAILURE;
  }
  
  // save the rig calibration (subposes)
  rig.saveCalibration(outputFile);
  
  
  // just print out the results
  // the first rig pose
  if(rig.getPosesSize() > 0)
  {
    POPART_COUT("First pose of the rig");
    const geometry::Pose3 &pose = rig.getPose(0); 
    POPART_COUT("R\n" << pose.rotation());
    POPART_COUT("center\n" << pose.center());
    POPART_COUT("t\n" << pose.translation());
  }
  
  // get the subposes of the cameras inside the rig
  const std::vector<geometry::Pose3>& subposes = rig.getRelativePoses();
  assert(nCam-1 == subposes.size());
  for(std::size_t i = 0; i < subposes.size(); ++i)
  {
    const geometry::Pose3 &pose = subposes[i];
    POPART_COUT("--------------------");
    POPART_COUT("Subpose p0" << i+1); // from camera 0 to camera i+1
    POPART_COUT("R\n" << pose.rotation());
    POPART_COUT("center\n" << pose.center());
    POPART_COUT("t\n" << pose.translation());
    POPART_COUT("--------------------\n");
  }
  
  return EXIT_SUCCESS;
}
