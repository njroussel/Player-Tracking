#include <opencv2/core/core_c.h>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc_c.h>
#include <functional>
#include "../headers/dpm_detector.h"
#include "../headers/debug.h"
#include "../headers/player_t.h"
#include "../headers/features_t.h"

namespace tmd {
    DPMDetector::DPMDetector(std::string model_file) {
        m_detector = cvLoadLatentSvmDetector(model_file.c_str());
        if (m_detector == NULL) {
            throw std::invalid_argument("Error in DPMDetector : couldn't create"
                                                " the detector.");
        }
        m_numthreads = TMD_DPM_DETECTOR_NUMTHREADS;
        int root_size_x = m_detector->filters[0]->sizeX;
        int root_size_y = m_detector->filters[0]->sizeY;
        tmd::debug("DPMDetector", "DPMDetector",
                   "Detector loaded, root filter size = (" +
                   std::to_string(root_size_x) + ", " +
                   std::to_string(root_size_y) + ").");
    }

    DPMDetector::~DPMDetector() {
        cvReleaseLatentSvmDetector(&m_detector); // Bug in CLion.
    }

    void DPMDetector::extractBodyParts(tmd::player_t *player) {
        if (player == NULL) {
            throw std::invalid_argument("Error in DPMDetector : NULL pointer in"
                                                " extractBodyParts method.");
        }
        IplImage playerImage = player->original_image; // Bug in CLion, ignore it.
        player->features.body_parts = getPartBoxesForImage(&playerImage);
        // If there is a player on the image, we compute its torso box.
        if (player->features.body_parts.size() > 0) {
            extractTorsoForPlayer(player);
        }
    }

    int DPMDetector::customEstimateBoxes(CvPoint *points, int *levels,
                                         int kPoints,
                                         int sizeX, int sizeY,
                                         CvPoint **oppositePoints) {
        int i;
        float step;

        step = powf(2.0f, 1.0f / ((float) (LAMBDA)));

        *oppositePoints = (CvPoint *) malloc(sizeof(CvPoint) * kPoints);
        for (i = 0; i < kPoints; i++) {
            getOppositePoint(points[i], sizeX, sizeY, step, levels[i] - LAMBDA,
                             &((*oppositePoints)[i]));
        }
        return LATENT_SVM_OK;
    }

    /*
    // Drawing part filter boxes
    //
    // API
    // int showPartFilterBoxes(const IplImage *image,
                               const filterObject *filter,
                               CvPoint *points, int *levels, int kPoints,
                               CvScalar color, int thickness,
                               int line_type, int shift);
    // INPUT
    // image             - initial image
    // filters           - a set of part filters
    // n                 - number of part filters
    // partsDisplacement - a set of points
    // levels            - levels of feature pyramid
    // kPoints           - number of foot filter positions
    // color             - line color for each box
    // thickness         - line thickness
    // line_type         - line type
    // shift             - shift
    // OUTPUT
    // parts             - reference on a vector ready to contain positions of
                                // all body parts.
    // window contained initial image and filter boxes
    // RESULT
    // Error status
    */
    int DPMDetector::detectBestPartBoxes(std::vector<cv::Rect> &parts,
                                         IplImage *image,
                                         const CvLSVMFilterObject **filters,
                                         int n,
                                         CvPoint **partsDisplacement,
                                         int *levels, int kPoints,
                                         float *scores) {
        int i, j;
        float step;
        CvPoint oppositePoint;

        step = powf(2.0f, 1.0f / ((float) LAMBDA));

        int max_level = 0;
        for (int i = 0; i < kPoints; i++) {
            if (levels[i] > max_level)
                max_level = levels[i];
        }
        float max_score_for_level = -2.f;
        for (int i = 0; i < kPoints; i++) {
            if (levels[i] == max_level &&
                scores[i] > max_score_for_level)
                max_score_for_level = scores[i];
        }

        for (i = 0; i < kPoints; i++) {
            for (j = 0; j < n; j++) {
                // Drawing rectangles for part filters
                getOppositePoint(partsDisplacement[i][j],
                                 filters[j + 1]->sizeX, filters[j + 1]->sizeY,
                                 step, levels[i] - 2 * LAMBDA, &oppositePoint);

                if (levels[i] == max_level &&
                    scores[i] == max_score_for_level) {
                    cv::Rect r(partsDisplacement[i][j], oppositePoint);
                    parts.push_back(r);
                }
            }
        }

        return LATENT_SVM_OK;
    }

    std::vector<cv::Rect> DPMDetector::getPartBoxesForImage(IplImage *image) {
        CvLSVMFeaturePyramid *H = 0;
        CvPoint *points = 0, *oppPoints = 0;
        int kPoints = 0;
        float *score = 0;
        unsigned int maxXBorder = 0, maxYBorder = 0;
        int numBoxesOut = 0;
        CvPoint *pointsOut = 0;
        CvPoint *oppPointsOut = 0;
        float *scoreOut = 0;
        CvSeq *result_seq = 0;
        int error = 0;

        if (image->nChannels == 3)
            cvCvtColor(image, image, CV_BGR2RGB);

        // Getting maximum filter dimensions
        getMaxFilterDims((const CvLSVMFilterObject **) (m_detector->filters),
                         m_detector->num_components,
                         m_detector->num_part_filters, &maxXBorder,
                         &maxYBorder);
        // Create feature pyramid with nullable border
        tmd::debug("DPMDetector", "getPartBoxesForImage",
                   "create featurePyramid.");
        H = createFeaturePyramidWithBorder(image, maxXBorder, maxYBorder);
        tmd::debug("DPMDetector", "getPartBoxesForImage", "done.");
        // Search object
        std::vector<cv::Rect> parts;
        error = preparePartDetection(parts, image, H,
                                     (const CvLSVMFilterObject **) (m_detector->filters),
                                     m_detector->num_components,
                                     m_detector->num_part_filters,
                                     m_detector->b,
                                     m_detector->score_threshold,
                                     &points, &oppPoints,
                                     &score, &kPoints,
                                     m_numthreads);
        if (error != LATENT_SVM_OK) {
            parts.clear();
            return parts;
        }

        if (image->nChannels == 3)
            cvCvtColor(image, image, CV_RGB2BGR);

        freeFeaturePyramidObject(&H);
        free(points);
        free(oppPoints);
        free(score);
        free(scoreOut);

        return parts;
    }


    /**
     * Redefinition of searchObjectThreshold from openCV.
     * This version is the exact same, but without the multi-threading support
     * which would return an error.
     */
    int customSearchObjectThreshold(const CvLSVMFeaturePyramid *H,
                                    const CvLSVMFilterObject **all_F, int n,
                                    float b,
                                    int maxXBorder, int maxYBorder,
                                    float scoreThreshold,
                                    CvPoint **points, int **levels,
                                    int *kPoints,
                                    float **score, CvPoint ***partsDisplacement,
                                    int numThreads) {
        int opResult;
        tmd::debug("DPMDetector", "customSearchObjectThreshold",
                   "call thresholdFunctionalScore()");
        opResult = thresholdFunctionalScore(all_F, n, H, b,
                                            maxXBorder, maxYBorder,
                                            scoreThreshold,
                                            score, points, levels,
                                            kPoints, partsDisplacement);
        tmd::debug("DPMDetector", "customSearchObjectThreshold", "done.");

        if (opResult != LATENT_SVM_OK) {
            return LATENT_SVM_SEARCH_OBJECT_FAILED;
        }

        // Transformation filter displacement from the block space
        // to the space of pixels at the initial image
        // that settles at the level number LAMBDA
        convertPoints(H->numLevels, LAMBDA, LAMBDA, (*points),
                      (*levels), (*partsDisplacement), (*kPoints), n,
                      maxXBorder, maxYBorder);

        return LATENT_SVM_OK;
    }


    int DPMDetector::preparePartDetection(std::vector<cv::Rect> &parts,
                                          IplImage *image,
                                          const CvLSVMFeaturePyramid *H,
                                          const CvLSVMFilterObject **filters,
                                          int kComponents,
                                          const int *kPartFilters,
                                          const float *b,
                                          float scoreThreshold,
                                          CvPoint **points,
                                          CvPoint **oppPoints,
                                          float **score,
                                          int *kPoints,
                                          int numThreads) {
        tmd::debug("DPMDetector", "preparePartDetection", "Entering method.");
        //int error = 0;
        int i, j, s, f, componentIndex;
        unsigned int maxXBorder, maxYBorder;
        CvPoint **pointsArr, **oppPointsArr, ***partsDisplacementArr;
        float **scoreArr;
        int *kPointsArr, **levelsArr;

        // Allocation memory
        pointsArr = (CvPoint **) malloc(sizeof(CvPoint *) * kComponents);
        oppPointsArr = (CvPoint **) malloc(sizeof(CvPoint *) * kComponents);
        scoreArr = (float **) malloc(sizeof(float *) * kComponents);
        kPointsArr = (int *) malloc(sizeof(int) * kComponents);
        levelsArr = (int **) malloc(sizeof(int *) * kComponents);
        partsDisplacementArr = (CvPoint ***) malloc(
                sizeof(CvPoint **) * kComponents);

        // Getting maximum filter dimensions
        /*error = */getMaxFilterDims(filters, kComponents, kPartFilters,
                                     &maxXBorder, &maxYBorder);
        componentIndex = 0;
        *kPoints = 0;
        // For each component perform searching
        int i_max = kComponents - 1;
        for (i = 0; i < kComponents; i++) {
            tmd::debug("DPMDetector", "preparePartDetection",
                       "Call searchObjectThreshold");
            int error = customSearchObjectThreshold(H,
                                                    &(filters[componentIndex]),
                                                    kPartFilters[i],
                                                    b[i], maxXBorder,
                                                    maxYBorder, scoreThreshold,
                                                    &(pointsArr[i]),
                                                    &(levelsArr[i]),
                                                    &(kPointsArr[i]),
                                                    &(scoreArr[i]),
                                                    &(partsDisplacementArr[i]),
                                                    numThreads);
            tmd::debug("DPMDetector", "preparePartDetection",
                       "searchObjectThreshold finished.");
            if (error != LATENT_SVM_OK) {
                // Release allocated memory
                free(pointsArr);
                free(oppPointsArr);
                free(scoreArr);
                free(kPointsArr);
                free(levelsArr);
                free(partsDisplacementArr);
                tmd::debug("DPMDetector", "preparePartDetection",
                           "searchObjectThreshold finished with error.");
                if (error == LATENT_SVM_TBB_NUMTHREADS_NOT_CORRECT) {
                    tmd::debug("DPMDetector", "preparePartDetection",
                               "error is LATENT_SVM_TBB_NUMTHREADS_NOT_CORRECT.");
                }
                return LATENT_SVM_SEARCH_OBJECT_FAILED;
            }
            customEstimateBoxes(pointsArr[i], levelsArr[i], kPointsArr[i],
                                filters[componentIndex]->sizeX,
                                filters[componentIndex]->sizeY,
                                &(oppPointsArr[i]));
            componentIndex += (kPartFilters[i] + 1);
            *kPoints += kPointsArr[i];
        }

        detectBestPartBoxes(parts, image, filters,
                            kPartFilters[i_max],
                            partsDisplacementArr[i_max],
                            levelsArr[i_max], kPointsArr[i_max],
                            scoreArr[i_max]);

        // Release allocated memory
        for (i = 0; i < kComponents; i++) {
            free(pointsArr[i]);
            free(oppPointsArr[i]);
            free(scoreArr[i]);
            free(levelsArr[i]);
            for (j = 0; j < kPointsArr[i]; j++) {
                free(partsDisplacementArr[i][j]);
            }
            free(partsDisplacementArr[i]);
        }
        free(pointsArr);
        free(oppPointsArr);
        free(scoreArr);
        free(kPointsArr);
        free(levelsArr);
        free(partsDisplacementArr);
        tmd::debug("DPMDetector", "preparePartDetection",
                   "Exiting preparePartDetection method.");
        return LATENT_SVM_OK;
    }

    int max(int a, int b){
        if (a < b) return b;
        else return a;
    }

    void DPMDetector::extractTorsoForPlayer(player_t *player) {
        if (player == NULL){
            throw std::invalid_argument("Error null pointer given to "
                                                "extractTorsoForPlayer method");
        }
        else if (player->features.body_parts.size() < 3){
            throw std::invalid_argument("Error not enough body parts in "
                                                "extractTorsoForPlayer");
        }
        cv::Rect torso1 = player->features.body_parts[1];
        cv::Rect torso2 = player->features.body_parts[2];
        cv::Rect mean;
        mean.x = (torso1.x + torso2.x) / 2;
        mean.y = (torso1.y + torso2.y) / 2;
        int oppoX = ((torso1.x + torso1.width) + (torso2.x + torso2.width))/2;
        int oppoY = ((torso1.y + torso1.height) + (torso2.y + torso2.height))/2;
        mean.width = oppoX - mean.x;
        mean.height = oppoY - mean.y;
        player->features.torso = player->original_image(mean);
        player->features.torso_mask = player->mask_image(mean);
        player->features.torso_pos = mean;
    }
}