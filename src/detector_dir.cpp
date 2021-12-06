#include <stdlib.h>
#include "darknet.h"
#include "network.h"
#include "region_layer.h"
#include "cost_layer.h"
#include "utils.h"
#include "parser.h"
#include "box.h"
#include "demo.h"
#include "option_list.h"
#include <iostream>
#include <algorithm>
#include <filesystem>

#ifndef __COMPAR_FN_T
#define __COMPAR_FN_T
typedef int (*__compar_fn_t)(const void*, const void*);
#ifdef __USE_GNU
typedef __compar_fn_t comparison_fn_t;
#endif
#endif

namespace fs = std::filesystem;

extern "C" {
void test_detector_dir(char *datacfg, char *cfgfile, char *weightfile, char *dirname, float thresh,
                       float hier_thresh, int dont_show, int ext_output, int save_labels, char *outfile, int letter_box,
                       int benchmark_layers) {
    list *options = read_data_cfg(datacfg);
    char *name_list = option_find_str(options, "names", "data/names.list");
    int names_size = 0;
    char **names = get_labels_custom(name_list, &names_size); //get_labels(name_list);

    image **alphabet = load_alphabet();
    network net = parse_network_cfg_custom(cfgfile, 1, 1); // set batch=1
    if (weightfile) {
        load_weights(&net, weightfile);
    }
    net.benchmark_layers = benchmark_layers;
    fuse_conv_batchnorm(net);
    calculate_binary_weights(net);
    if (net.layers[net.n - 1].classes != names_size) {
        printf("\n Error: in the file %s number of names %d that isn't equal to classes=%d in the file %s \n",
               name_list, names_size, net.layers[net.n - 1].classes, cfgfile);
        if (net.layers[net.n - 1].classes > names_size) getchar();
    }
    srand(2222222);
    char buff[256];
    char *input = buff;
    char *json_buf = NULL;
    int json_image_id = 0;
    FILE *json_file = NULL;
    if (outfile) {
        json_file = fopen(outfile, "wb");
        if (!json_file) {
            error("fopen failed", DARKNET_LOC);
        }
        char *tmp = "[\n";
        fwrite(tmp, sizeof(char), strlen(tmp), json_file);
    }
    int cnt = 0;
    float nms = .45;    // 0.4F
    printf("opening %s\n", dirname);
    std::vector<std::filesystem::path> files_list;
    std::copy(std::filesystem::recursive_directory_iterator(dirname), std::filesystem::recursive_directory_iterator(), std::back_inserter(files_list));
    std::sort(files_list.begin(), files_list.end());
    //for (auto &p: fs::recursive_directory_iterator(dirname)) {
    for (auto &p: files_list) {
        //FILE *fp = fopen(listfile, "r");
        //while (1) {
        //char filename[512];
        //sprintf(filename, p.path().c_str());
        //std::cout << filename << std::endl;
        //filename[strlen(filename) - 1] = '\0';
        strncpy(input, p.c_str(), 256);
        if (p.extension().string() != std::string(".jpg") &&
            p.extension().string() != std::string(".png") &&
            p.extension().string() != std::string(".jpeg")) {
            continue;
        }
        std::cout << input << std::endl;
        if (strlen(input) > 0) {
            if (input[strlen(input) - 1] == 0x0d) input[strlen(input) - 1] = 0;
        }
        //image im;
        //image sized = load_image_resize(input, net.w, net.h, net.c, &im);
        image im = load_image(input, 0, 0, net.c);
        image sized;
        if (letter_box) sized = letterbox_image(im, net.w, net.h);
        else sized = resize_image(im, net.w, net.h);

        layer l = net.layers[net.n - 1];
        for (int k = 0; k < net.n; ++k) {
            layer lk = net.layers[k];
            if (lk.type == YOLO || lk.type == GAUSSIAN_YOLO || lk.type == REGION) {
                l = lk;
                printf(" Detection layer: %d - type = %d \n", k, l.type);
            }
        }

        //box *boxes = calloc(l.w*l.h*l.n, sizeof(box));
        //float **probs = calloc(l.w*l.h*l.n, sizeof(float*));
        //for(j = 0; j < l.w*l.h*l.n; ++j) probs[j] = (float*)xcalloc(l.classes, sizeof(float));

        float *X = sized.data;

        //time= what_time_is_it_now();
        double time = get_time_point();
        network_predict(net, X);
        //network_predict_image(&net, im); letterbox = 1;
        printf("%s: Predicted in %lf milli-seconds.\n", input, ((double) get_time_point() - time) / 1000);
        //printf("%s: Predicted in %f seconds.\n", input, (what_time_is_it_now()-time));

        int nboxes = 0;
        detection *dets = get_network_boxes(&net, im.w, im.h, thresh, hier_thresh, 0, 1, &nboxes, letter_box);
        if (nms) {
            if (l.nms_kind == DEFAULT_NMS) do_nms_sort(dets, nboxes, l.classes, nms);
            else diounms_sort(dets, nboxes, l.classes, nms, l.nms_kind, l.beta_nms);
        }
        draw_detections_v3(im, dets, nboxes, thresh, names, alphabet, l.classes, ext_output);
        char filename[256];
        sprintf(filename, "pred%04d_%s", cnt++, p.stem().c_str());
        save_image(im, filename);
        if (!dont_show) {
            show_image(im, "predictions");
        }

        if (json_file) {
            if (json_buf) {
                char *tmp = ", \n";
                fwrite(tmp, sizeof(char), strlen(tmp), json_file);
            }
            ++json_image_id;
            json_buf = detection_to_json(dets, nboxes, l.classes, names, json_image_id, input);

            fwrite(json_buf, sizeof(char), strlen(json_buf), json_file);
            free(json_buf);
        }

        // pseudo labeling concept - fast.ai
        if (save_labels) {
            char labelpath[4096];
            replace_image_to_label(input, labelpath);

            FILE *fw = fopen(labelpath, "wb");
            for (int i = 0; i < nboxes; ++i) {
                char buff[1024];
                int class_id = -1;
                float prob = 0;
                for (int j = 0; j < l.classes; ++j) {
                    if (dets[i].prob[j] > thresh && dets[i].prob[j] > prob) {
                        prob = dets[i].prob[j];
                        class_id = j;
                    }
                }
                if (class_id >= 0) {
                    sprintf(buff, "%d %2.4f %2.4f %2.4f %2.4f\n", class_id, dets[i].bbox.x, dets[i].bbox.y,
                            dets[i].bbox.w, dets[i].bbox.h);
                    fwrite(buff, sizeof(char), strlen(buff), fw);
                }
            }
            fclose(fw);
        }

        free_detections(dets, nboxes);
        free_image(im);
        free_image(sized);

        if (!dont_show) {
            wait_until_press_key_cv();
            destroy_all_windows_cv();
        }
    }

    if (json_file) {
        char *tmp = "\n]";
        fwrite(tmp, sizeof(char), strlen(tmp), json_file);
        fclose(json_file);
    }

    // free memory
    free_ptrs((void **) names, net.layers[net.n - 1].classes);
    free_list_contents_kvp(options);
    free_list(options);

    const int nsize = 8;
    for (int j = 0; j < nsize; ++j) {
        for (int i = 32; i < 127; ++i) {
            free_image(alphabet[j][i]);
        }
        free(alphabet[j]);
    }
    free(alphabet);

    free_network(net);
}

} // extern "C"