#include "texturing.h"
#include <util/tokenizer.h>
#include <mve/image_io.h>
#include <mve/image_tools.h>
#include <mve/bundle_io.h>

void
from_mve_scene(std::string const & scene_dir, std::string const & embedding_name,
    std::vector<TextureView> * texture_views) {

    mve::Scene::Ptr scene;
    try {
        scene = mve::Scene::create(scene_dir);
    } catch (std::exception& e) {
        std::cerr << "Could not open scene: " << e.what() << std::endl;
        std::exit(EXIT_FAILURE);
    }
    std::size_t num_views = scene->get_views().size();
    texture_views->reserve(num_views);

    ProgressCounter view_counter("\tLoading", num_views);
    for (std::size_t i = 0; i < num_views; ++i) {
        view_counter.progress<SIMPLE>();

        mve::View::Ptr view = scene->get_view_by_id(i);
        if (view == NULL) {
            view_counter.inc();
            continue;
        }

        if (!view->has_embedding(embedding_name)) {
            std::cerr << view->get_name() << " has no embedding "
                << embedding_name << std::endl;
            exit(EXIT_FAILURE);
        }

        if (view->get_byte_image(embedding_name) == NULL) {
            std::cerr << "Embedding " << embedding_name << " of view " <<
                view->get_name() << " is not a byte embedding " << std::endl;
            exit(EXIT_FAILURE);
        }

        mve::ByteImage::Ptr image = view->get_byte_image(embedding_name);

        texture_views->push_back(TextureView(view->get_id(), view->get_camera(), image));
        view_counter.inc();
    }
}

void
from_images_and_camera_files(std::string const & path, std::vector<TextureView> * texture_views) {
    util::fs::Directory dir(path);
    std::sort(dir.begin(), dir.end());
    std::vector<std::string> files;
    for (std::size_t i = 0; i < dir.size(); ++i) {
        util::fs::File const & cam_file = dir[i];
        if (cam_file.is_dir) continue;

        std::string cam_file_ext = util::string::uppercase(util::string::right(cam_file.name, 4));
        if (cam_file_ext != ".CAM") continue;

        std::string prefix = util::string::left(cam_file.name, cam_file.name.size() - 4);
        if (prefix.empty()) continue;

        /* Find corresponding image file. */
        int step = 1;
        for (std::size_t j = i + 1; 0 <= j && j < dir.size(); j += step) {
            util::fs::File const & img_file = dir[j];

            /* Since the files are sorted we can break - no more files with the same prefix exist. */
            if (util::string::left(img_file.name, prefix.size()) != prefix) {
                if (step == 1) {
                    j = i;
                    step = -1;
                    continue;
                } else {
                    break;
                }
            }

            /* Image file (based on extension)? */
            std::string img_file_ext = util::string::uppercase(util::string::right(img_file.name, 4));
            if (img_file_ext != ".PNG" && img_file_ext != ".JPG" &&
                img_file_ext != "TIFF" && img_file_ext != "JPEG") continue;

            files.push_back(cam_file.get_absolute_name());
            files.push_back(img_file.get_absolute_name());
            break;
        }
    }

    ProgressCounter view_counter("\tLoading", files.size() / 2);
    #pragma omp parallel for
    for (std::size_t i = 0; i < files.size(); i += 2) {
        view_counter.progress<SIMPLE>();
        std::string cam_file = files[i];
        std::string img_file = files[i + 1];

        mve::ByteImage::Ptr image = mve::image::load_file(img_file);

        /* Read CAM file. */
        std::ifstream infile(cam_file.c_str(), std::ios::binary);
        if (!infile.good())
            throw util::FileException(util::fs::basename(cam_file), std::strerror(errno));
        std::string cam_int_str, cam_ext_str;
        std::getline(infile, cam_ext_str);
        std::getline(infile, cam_int_str);
        util::Tokenizer tok_ext, tok_int;
        tok_ext.split(cam_ext_str);
        tok_int.split(cam_int_str);
        #pragma omp critical
        if (tok_ext.size() != 12 || tok_int.size() < 1) {
            std::cerr << "Invalid CAM file: " << util::fs::basename(cam_file) << std::endl;
            std::exit(EXIT_FAILURE);
        }

        /* Create cam_info and eventually undistort image. */
        mve::CameraInfo cam_info;
        cam_info.from_ext_string(cam_ext_str);
        cam_info.from_int_string(cam_int_str);

        mve::ByteImage::Ptr undist;
        if (cam_info.dist[0] != 0.0f) {
            if (cam_info.dist[1] != 0.0f) {
                undist = mve::image::image_undistort_bundler<uint8_t>(image,
                    cam_info.flen, cam_info.dist[0], cam_info.dist[1]);
            } else {
                undist = mve::image::image_undistort_vsfm<uint8_t>(image,
                    cam_info.flen, cam_info.dist[0]);
            }
        } else {
            undist = image;
        }

        mve::image::save_png_file(undist, std::string("/tmp/") + util::fs::basename(img_file));

        #pragma omp critical
        texture_views->push_back(TextureView(i / 2, cam_info, undist));
        view_counter.inc();
    }
}

void
from_nvm_scene(std::string const & nvm_file, std::vector<TextureView> * texture_views) {
    std::vector<mve::NVMCameraInfo> nvm_cams;
    mve::Bundle::Ptr bundle = mve::load_nvm_bundle(nvm_file, &nvm_cams);
    mve::Bundle::Cameras& cameras = bundle->get_cameras();

    ProgressCounter view_counter("\tLoading", cameras.size());
    #pragma omp parallel for
    for (std::size_t i = 0; i < cameras.size(); ++i) {
        view_counter.progress<SIMPLE>();
        mve::CameraInfo& mve_cam = cameras[i];
        mve::NVMCameraInfo const& nvm_cam = nvm_cams[i];

        mve::ByteImage::Ptr image = mve::image::load_file(nvm_cam.filename);

        int const maxdim = std::max(image->width(), image->height());
        mve_cam.flen = mve_cam.flen / static_cast<float>(maxdim);

        image = mve::image::image_undistort_vsfm<uint8_t>
            (image, mve_cam.flen, nvm_cam.radial_distortion);

        texture_views->push_back(TextureView(i, mve_cam, image));
        view_counter.inc();
    }
}

void
generate_texture_views(Arguments const & conf, std::vector<TextureView> * texture_views) {
    util::Tokenizer tok;
    tok.split(conf.in_scene, ':', true);

    /* Determine input format. */

    /* BUNDLEFILE */
    if (tok.size() == 1 && util::fs::file_exists(tok[0].c_str())) {
        std::string const & file = tok[0];
        std::string extension = util::string::uppercase(util::string::right(file, 3));
        if (extension == "NVM") from_nvm_scene(file, texture_views);
    }

    /* SCENE_FOLDER */
    if (tok.size() == 1 && util::fs::dir_exists(tok[0].c_str())) {
        from_images_and_camera_files(tok[0], texture_views);
    }

    /* MVE_SCENE::EMBEDDING */
    if (tok.size() == 3 && tok[1].empty()) {
        from_mve_scene(tok[0], tok[2], texture_views);
    }

    std::size_t num_views = texture_views->size();
    if (num_views == 0) {
        std::cerr << "No proper input scene descriptor given." << std::endl
            << "A input descriptor can be:" << std::endl
            << "BUNDLE_FILE - a bundle file (currently onle .nvm files are supported)" << std::endl
            << "SCENE_FOLDER - a folder containing images and .cam files" << std::endl
            << "MVE_SCENE::EMBEDDING - a mve scene and embedding" << std::endl;
        exit(EXIT_FAILURE);
    }

    /* We can skip these steps if the view selection does not need to be run. */
    if (conf.labeling_file.empty()) {
        ProgressCounter view_counter("\tGenerating validity masks", num_views);
        #pragma omp parallel for
        for (std::size_t i = 0; i < num_views; ++i) {
            view_counter.progress<SIMPLE>();
            texture_views->at(i).generate_validity_mask();
            view_counter.inc();
        }

        view_counter.reset("\tCalculating gradient magnitude");
        #pragma omp parallel for
        for (std::size_t i = 0; i < num_views; ++i) {
            view_counter.progress<SIMPLE>();
            texture_views->at(i).generate_gradient_magnitude();
            texture_views->at(i).erode_validity_mask();
            view_counter.inc();
        }
    }
}
