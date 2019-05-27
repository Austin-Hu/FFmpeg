def fit_size(image):
    """
    If image's width or height is not multiples of 8,
    add padding to width or height, to make them multiples of 8.
    :param image: input image
    :return: output_image, pad_width, pad_height
    """
    h, w, c = image.shape
    assert c == 3
    pad_h, pad_w = 0, 0
    
    remains = h % 8
    if remains != 0:
        pad_h = 8 - remains

    remains = w % 8
    if remains != 0:
        pad_w = 8 - remains
            
    if pad_h == 0 and pad_w == 0:
        return image, 0, 0
            
    new_h = h + pad_h
    new_w = w + pad_w
    padded_image = np.zeros((new_h, new_w, c), image.dtype)
    padded_image[:h, :w, :] = image
    return padded_image, pad_w, pad_h


def get_normed_edge_feature(im):
    im = cv2.cvtColor(im, cv2.COLOR_BGR2GRAY)
    grad_x = cv2.Sobel(im, cv2.CV_32F, 1, 0, None, 3)
    grad_x = np.abs(grad_x)
    grad_y = cv2.Sobel(im, cv2.CV_32F, 0, 1, None, 3)
    grad_y = np.abs(grad_y)
    total_grad = cv2.addWeighted(grad_x, 0.5, grad_y, 0.5, 0)
    total_grad = cv2.normalize(total_grad, None, alpha=0.0, beta=1.0, norm_type=cv2.NORM_MINMAX)
    total_grad -= 0.5
    return total_grad


def convert(image):
    image = image / 256.0
    image = image - 0.5
    image = np.transpose(image, (2, 0, 1))
    data = image.astype(np.float32)
    data = data[np.newaxis, :, :, :]
    return data


def predict(seg_net, image, thresh=0.5):
    original_height, original_width, _ = image.shape
    if original_height > max_input_height or original_width > max_input_width:
        h_ratio = max_input_height * 1.0 / original_height
        w_ratio = max_input_width * 1.0 / original_width
        ratio = min(h_ratio, w_ratio)
        rescaled_image = cv2.resize(image, None, fx=ratio, fy=ratio)
    else:
        rescaled_image = image
    
    fit_size_image, pad_w, pad_h = fit_size(rescaled_image)
    input_h, input_w, _ = fit_size_image.shape
    seg_net.blobs['data'].reshape(1, 3, input_h, input_w)
    seg_net.blobs['edge_feature'].reshape(1, 1, input_h, input_w)
    
    for _ in range(1):
        
        input_data = convert(fit_size_image)
        edge_feature = get_normed_edge_feature(fit_size_image)
        seg_net.blobs['data'].data[...] = input_data
        seg_net.blobs['edge_feature'].data[...] = edge_feature[np.newaxis, np.newaxis, :, :]
        tic = time.time()
        output = seg_net.forward()
        toc = time.time()
        print('seg time total = {:f}.'.format(toc - tic))
    
    seg = output['seg_out'][0]
    seg = np.squeeze(seg)
    
    seg[seg > thresh] = person_label
    seg[seg != person_label] = 0
    seg = seg.astype(np.uint8)

    if pad_h > 0:
        seg = seg[:-pad_h, :]
    if pad_w > 0:
        seg = seg[:, :-pad_w]
    
    mask = cv2.resize(seg, (original_width, original_height))
    
    if np.count_nonzero(mask) > 0:
        image[mask == 0] = 0
    
    cv2.imwrite('segmentation.png', image)
    
seg_net = ''
person_label = 255
max_input_height = 480
max_input_width = 640

def init():
    import caffe
    import numpy as np
    import cv2
    import sys
    import time
    import lmdb
    from caffe.proto import caffe_pb2

    seg_prototxt = 'libavfilter/person_seg_net_6.0_deploy.prototxt'
    seg_weights = 'libavfilter/person_seg_net_6.0_iter_940000.caffemodel'

    caffe.set_mode_gpu()
    caffe.set_device(1)

    seg_net = caffe.Net(seg_prototxt, seg_weights, caffe.TEST)
    print('Segment model is initialized.')

def segment():
    import cv2
    img = cv2.imread('frames_140.bmp')
    predict(seg_net, img, float(0.5))
