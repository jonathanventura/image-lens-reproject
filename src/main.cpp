#define CXXOPTS_NO_REGEX 1
#include <Tracy.hpp>
#include <cxxopts.hpp>

#include "image_formats.hpp"
#include "reproject.hpp"
#include <atomic>
#include <cmath>
#include <ctpl_stl.h>
#include <ghc/filesystem.hpp>
#include <nlohmann/json.hpp>

namespace fs = ghc::filesystem;

int main(int argc, char **argv) {
  // clang-format off
  cxxopts::Options options(argv[0],
    "Reprojection tool for producing a variation of lens\n"
    "configurations based on one reference image given a\n"
    "known lens configuration.");
  options.add_options("Input/output")
    ("input-cfg", "Input JSON file containing lens and camera "
                  "settings of the input images.",
     cxxopts::value<std::string>(), "json-file")
    ("output-cfg", "Output JSON file containing lens and camera "
                   "settings of the input images.",
     cxxopts::value<std::string>(), "json-file")
    ("i,input-dir", "Input directory containing images to reproject.",
     cxxopts::value<std::string>(), "file")
    ("single", "A single input file to convert.",
     cxxopts::value<std::string>(), "file")
    ("o,output-dir", "Output directory to put the reprojected images.",
     cxxopts::value<std::string>(), "file")
    ("exr", "Output EXR files. Color and depth.")
    ("png", "Output PNG files. Color only.")
    ;

  options.add_options("Filter files")
    ("filter-prefix", "Only include files starting with",
     cxxopts::value<std::string>()->default_value(""), "prefix")
    ("filter-suffix", "Only include files ending with",
     cxxopts::value<std::string>()->default_value(""), "suffix")
    ;

  options.add_options("Sampling")
    ("s,samples", "Number of samples per dimension for interpolating",
     cxxopts::value<int>()->default_value("1"), "number")

    ("nn", "Nearest neighbor interpolation")
    ("bl", "Bilinear interpolation")
    ("bc", "Bicubic interpolation (default)")

    ("scale", "Output scale, as a fraction of the input size. "
     "It is recommended to increase --samples to prevent aliassing "
     "in case you are downscaling. Eg: --scale 0.5 --samples 2 "
     "or --scale 0.33334 --samples 3 or --scale 0.25 --samples 4. "
     "Final dimensions are rounded towards zero.",
     cxxopts::value<double>()->default_value("1.0"), "percentage")
    ;

  options.add_options("Output optics")
    ("no-reproject", "Do not reproject at all.")
    ("rectilinear", "Output rectilinear images with given "
                    "focal_length,sensor_width tuple.",
     cxxopts::value<std::string>(), "focal_length,sensor_width")
    ("equisolid", "Output equisolid images with given "
                  "focal_length,sensor_width,fov tuple.",
     cxxopts::value<std::string>(), "focal_length,sensor_width,fov")
    ("equidistant", "Output equidistant images with given "
                    "fov value.",
     cxxopts::value<std::string>(), "fov")
    ;

  options.add_options("Color processing")
    ("auto-exposure", "Automatic exposure compensation and white balance.",
     cxxopts::value<bool>(), "auto_exposure")
    ("exposure", "Exposure compensation in stops (EV) to brigthen "
                 "or darken the pictures.",
     cxxopts::value<double>()->default_value("0.0"), "EV")
    ("reinhard", "Use reinhard tonemapping with given maximum value "
                 "(after exposure processing) on the output images.",
     cxxopts::value<double>()->default_value("1.0"), "max")
    ;


  options.add_options("Runtime")
    ("skip-if-exists", "Skip if the output file already exists.")
    ("j,parallel", "Number of parallel images to process.",
     cxxopts::value<int>()->default_value("1"), "threads")
    ("dry-run", "Do not actually reproject images. Only produce config.")
    ("h,help", "Show help")
    ;
  // clang-format on

  cxxopts::ParseResult result;
  int num_threads = 1;
  int num_samples = 1;
  std::string input_single;
  std::string input_dir;
  std::string output_dir;
  std::string input_cfg_file;
  std::string output_cfg_file;
  double scale;
  bool auto_exposure = false;
  double exposure = 1.0;
  double reinhard = 1.0;
  bool dry_run = false;
  bool reproject = true;
  bool skip_if_exists = false;
  try {
    result = options.parse(argc, argv);
    if (result.count("help")) {
      std::printf("%s\n", options.help().c_str());
      return 0;
    }
    if (result.count("input-dir") && result.count("single")) {
      std::printf("Error: cannot specify both --input-dir and --single.\n");
      std::printf("%s\n", options.help().c_str());
      return 1;
    } else {
      if (result.count("input-dir")) {
        input_dir = result["input-dir"].as<std::string>();
      } else if (result.count("single")) {
        input_single = result["single"].as<std::string>();
      } else {
        std::printf("Error: No input specified.\n");
        return 1;
      }
    }
    output_dir = result["output-dir"].as<std::string>();
    input_cfg_file = result["input-cfg"].as<std::string>();
    output_cfg_file = result["output-cfg"].as<std::string>();
    num_samples = result["samples"].as<int>();
    num_threads = result["parallel"].as<int>();
    scale = result["scale"].as<double>();
    auto_exposure = result["auto-exposure"].as<bool>();
    exposure = std::pow(2.0, result["exposure"].as<double>());
    reinhard = result["reinhard"].as<double>();
    if (result.count("no-reproject")) {
      reproject = false;
    }
  } catch (cxxopts::OptionParseException &e) {
    std::printf("%s\n\n%s\n", e.what(), options.help().c_str());
    return 1;
  } catch (cxxopts::OptionException &e) {
    std::printf("%s\n\n%s\n", e.what(), options.help().c_str());
    return 1;
  }

  if (result.count("dry-run")) {
    dry_run = true;
  }
  if (result.count("skip-if-exists")) {
    skip_if_exists = true;
  }

  bool store_png = false;
  bool store_exr = false;
  if (result.count("exr")) {
    store_exr = true;
  }
  if (result.count("png")) {
    store_png = true;
  }

  if (!store_exr && !store_png) {
    std::printf("Error: Did not specify any output format.\n"
                "Choose --png or --exr. (both are possible).\n");
    return 1;
  }

  reproject::Interpolation interpolation = reproject::BICUBIC;
  int found_interpolation_flag = 0;
  if (result.count("nn")) {
    found_interpolation_flag++;
    interpolation = reproject::NEAREST;
  }
  if (result.count("bl")) {
    found_interpolation_flag++;
    interpolation = reproject::BILINEAR;
  }
  if (result.count("bc")) {
    found_interpolation_flag++;
    interpolation = reproject::BICUBIC;
  }
  if (found_interpolation_flag > 1) {
    std::printf("Cannot specify more than one interpolation method.\n\n");
    std::printf("%s", options.help().c_str());
  }

  std::string filter_prefix = result["filter-prefix"].as<std::string>();
  std::string filter_suffix = result["filter-suffix"].as<std::string>();

  nlohmann::json cfg;
  std::ifstream cfg_ifstream{input_cfg_file};
  cfg_ifstream >> cfg;
  cfg_ifstream.close();

  nlohmann::json out_cfg = cfg;
  for (int i = 0; i < out_cfg["frames"].size(); ++i) {
    std::string name = out_cfg["frames"][i]["name"].get<std::string>();

    bool remove = false;
    if (name.size() < filter_prefix.size() ||
        name.size() < filter_suffix.size()) {
      remove = true;
    } else if (name.substr(0, filter_prefix.size()) != filter_prefix) {
      remove = true;
    } else if (name.substr(name.size() - filter_suffix.size()) !=
               filter_suffix) {
      remove = true;
    }
    if (remove) {
      out_cfg["frames"].erase(i--);
    }
  }

  std::printf("Found camera config: %s\n", cfg["camera"].dump(1).c_str());
  int res_x = cfg["resolution"][0].get<int>();
  int res_y = cfg["resolution"][1].get<int>();

  reproject::LensInfo input_lens =
      reproject::extract_lens_info_from_config(cfg);
  reproject::LensInfo output_lens;
  int output_lens_types_found = 0;
  if (result.count("rectilinear")) {
    std::string lstr = result["rectilinear"].as<std::string>();
    int comma = lstr.find(",");
    if (comma == std::string::npos) {
      std::printf("Error: Required format for --rectilinear x,y\n");
      return 1;
    }
    auto &ol = output_lens;
    auto &olr = output_lens.rectilinear;
    output_lens.type = reproject::RECTILINEAR;
    olr.focal_length = std::atof(lstr.substr(0, comma).c_str());
    ol.sensor_width = std::atof(lstr.substr(comma + 1).c_str());
    ol.sensor_height = (float)res_y / (float)res_x * ol.sensor_width;

    output_lens_types_found++;
  }
  if (result.count("equisolid")) {
    std::string lstr = result["equisolid"].as<std::string>();
    int comma1 = lstr.find(",");
    int comma2 = lstr.find(",", comma1 + 1);
    if (comma1 == std::string::npos || comma2 == std::string::npos) {
      std::printf("Error: Required format for --equisolid x,y,z\n");
      return 1;
    }
    auto &ol = output_lens;
    auto &olfes = output_lens.fisheye_equisolid;
    output_lens.type = reproject::FISHEYE_EQUISOLID;
    olfes.focal_length = std::atof(lstr.substr(0, comma1).c_str());
    olfes.fov = std::atof(lstr.substr(comma2 + 1).c_str());
    ol.sensor_width = std::atof(lstr.substr(comma1 + 1, comma2).c_str());
    ol.sensor_height = (float)res_y / (float)res_x * ol.sensor_width;

    output_lens_types_found++;
  }
  if (result.count("equidistant")) {
    std::string lstr = result["equidistant"].as<std::string>();
    auto &ol = output_lens;
    auto &olfed = output_lens.fisheye_equidistant;
    output_lens.type = reproject::FISHEYE_EQUISOLID;
    olfed.fov = std::atof(lstr.c_str());
    ol.sensor_width = 36.0f;
    ol.sensor_height = 36.0f;

    output_lens_types_found++;
  }

  if (!reproject) {
    output_lens = input_lens;
    output_lens_types_found++;
  }

  // store in out_cfg
  reproject::store_lens_info_in_config(output_lens, out_cfg);
  cfg["resolution"][0] = int(res_x * scale);
  cfg["resolution"][1] = int(res_y * scale);

  if (output_lens_types_found > 1) {
    std::printf("Error: only specify one output lens type: [--rectilinear, "
                "--equisolid, --equidistant, --no-reproject].\n");
    return 1;
  }

  std::printf("Creating directory: %s\n", output_dir.c_str());
  fs::create_directory(output_dir);

  std::printf("Saving output config: %s\n", output_cfg_file.c_str());
  std::ofstream cfg_ofstream{output_cfg_file};
  cfg_ofstream << out_cfg.dump(2);
  cfg_ofstream.close();

  if (dry_run) {
    std::printf("Dry-run. Exiting.\n");
    return 0;
  }

  int count = 0;
  std::atomic_int done_count{0};
  ctpl::thread_pool pool(num_threads);

  std::function<void(std::string)> submit_file = [&](fs::path p) {
    pool.push([p, num_samples, interpolation, output_dir, scale, input_lens,
               output_lens, &done_count, &count, reproject, auto_exposure, exposure, reinhard,
               store_exr, store_png, skip_if_exists](int) {
      ZoneScopedN("process_file");
      try {
        fs::path output_path_base = output_dir / p.filename();
        fs::path output_path_png = output_path_base.replace_extension(".png");
        fs::path output_path_exr = output_path_base.replace_extension(".exr");

        bool exists = true;
        if (store_png && !fs::exists(output_path_png)) {
          exists = false;
        }
        if (store_exr && !fs::exists(output_path_exr)) {
          exists = false;
        }
        if (exists && skip_if_exists) {
          std::printf("Skipping '%s'. Already exists.\n",
                      output_path_png.c_str());
          done_count++;
          return;
        }

        reproject::Image input;
        if (p.extension() == ".exr") {
          input = reproject::read_exr(p.string());
        } else if (p.extension() == ".png") {
          input = reproject::read_png(p.string());
        }
        input.lens = input_lens;

        reproject::Image output;
        output.lens = output_lens;

        output.width = int(input.width * scale);
        output.height = int(input.height * scale);
        output.channels = input.channels;
        output.data_layout = input.data_layout;
        output.data = new float[output.width * output.height * output.channels];

        if (!reproject && scale == 1.0) {
          uint64_t bytes = output.width * output.height;
          bytes *= output.channels * sizeof(float);
          std::memcpy(output.data, input.data, bytes);
        } else {
          reproject::reproject(&input, &output, num_samples, interpolation);
        }

        if (auto_exposure) {
          reproject::auto_exposure(&output, reinhard);
        } else if (exposure != 1.0 || reinhard != 1.0) {
          reproject::post_process(&output, exposure, reinhard);
        }

        if (store_png) {
          reproject::save_png(output, output_path_png.string());
        }
        if (store_exr) {
          reproject::save_exr(output, output_path_exr.string());
        }

        delete[] input.data;
        delete[] output.data;

        int dc = ++done_count;
        std::printf("%4d / %4d: %s\n", dc, count, p.stem().c_str());
      } catch (const std::exception &e) {
        std::printf("Error: %s\n", e.what());
      }
    });
    count++;
  };

  if (!input_dir.empty()) {
    fs::directory_iterator end;
    fs::directory_iterator it{fs::path(input_dir)};

    std::vector<fs::path> paths;
    for (; it != end; ++it) {
      if (it->is_regular_file()) {
        paths.push_back(*it);
      }
    }
    std::sort(paths.begin(), paths.end());

    for (fs::path &p : paths) {
      std::string fn = p.filename().string();
      if (fn.size() < filter_prefix.size() ||
          fn.size() < filter_suffix.size()) {
        continue;
      }
      if (fn.substr(0, filter_prefix.size()) != filter_prefix) {
        continue;
      }
      if (fn.substr(fn.size() - filter_suffix.size()) != filter_suffix) {
        continue;
      }
      if (p.extension() == ".exr" || p.extension() == ".png") {
        submit_file(p);
      }
    }
  } else if (!input_single.empty()) {
    fs::path p{input_single};
    submit_file(p);
  }

  pool.stop(true);

  return 0;
}
