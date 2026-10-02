/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "fsmn.h"
#include <cstdio>
#include <cstring>

#include "model_io.hpp"

namespace kws::cfsmn {

namespace {

void matvec(const float *W, const float *b, const float *x, float *y, int out, int in) {
    for (int o = 0; o < out; ++o) {
        const float *w = W + (size_t)o * in;
        float s = b ? b[o] : 0.f;
        for (int i = 0; i < in; ++i) s += w[i] * x[i];
        y[o] = s;
    }
}
void relu(float *x, int n) { for (int i = 0; i < n; ++i) if (x[i] < 0) x[i] = 0; }

void front(const Model &m, const float *x, float *h, float *ti, float *ta) {
    for (int i = 0; i < m.idim; ++i) ti[i] = (x[i] - m.mean[i]) * m.istd[i];
    matvec(m.in1w, m.in1b, ti, ta, m.a1, m.idim);
    matvec(m.in2w, m.in2b, ta, h, m.ldim, m.a1);
    relu(h, m.ldim);
}
void back(const Model &m, const float *h, float *logits, float *ta) {
    matvec(m.o1w, m.o1b, h, ta, m.a2, m.ldim);      // no relu between the two output affines
    matvec(m.o2w, m.o2b, ta, logits, m.odim, m.a2);
}

}  // namespace

bool Model::load(const char *path) {
    *this = Model{};
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return false; }
    char magic[4];
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "KWSF", 4)) {
        fprintf(stderr, "%s: bad magic\n", path);
        fclose(f);
        return false;
    }
    uint32_t d[9];
    for (auto &value : d) {
        if (!readLe32(f, value) || value > 8192) { fclose(f); return false; }
    }
    if (!d[0] || !d[1] || !d[2] || !d[3] || !d[4] || !d[6] || !d[7] || !d[8] ||
        d[4] > 256 || d[5] > 256 || d[6] > 64) { fclose(f); return false; }
    idim = d[0]; a1 = d[1]; ldim = d[2]; pdim = d[3];
    lorder = d[4]; rorder = d[5]; layers = d[6]; a2 = d[7]; odim = d[8];
    const size_t n = 2ULL * idim + (size_t)a1 * idim + a1 + (size_t)ldim * a1 + ldim
                    + layers * ((size_t)pdim * ldim + (size_t)pdim * lorder + (size_t)pdim * rorder
                                + (size_t)ldim * pdim + ldim)
                    + (size_t)a2 * ldim + a2 + (size_t)odim * a2 + odim;
    const bool ok = readModelFloats(f, n, blob);
    fclose(f);
    if (!ok) { *this = Model{}; return false; }
    const float *p = blob.data();
    mean = p; p += idim;  istd = p; p += idim;
    in1w = p; p += (long)a1 * idim;  in1b = p; p += a1;
    in2w = p; p += (long)ldim * a1;  in2b = p; p += ldim;
    L.resize(layers);
    for (int i = 0; i < layers; ++i) {
        L[i].pw = p; p += (long)pdim * ldim;
        L[i].cl = p; p += (long)pdim * lorder;
        L[i].cr = p; p += (long)pdim * rorder;
        L[i].aw = p; p += (long)ldim * pdim;
        L[i].ab = p; p += ldim;
    }
    o1w = p; p += (long)a2 * ldim;  o1b = p; p += a2;
    o2w = p; p += (long)odim * a2;  o2b = p; p += odim;
    return static_cast<size_t>(p - blob.data()) == n;
}

void forward_window(const Model &m, const float *X, int T, float *logits) {
    std::vector<float> h((size_t)T * m.ldim), p((size_t)T * m.pdim), q((size_t)T * m.pdim);
    std::vector<float> ti(m.idim), ta(m.a1 > m.a2 ? m.a1 : m.a2);
    for (int t = 0; t < T; ++t)
        front(m, X + (size_t)t * m.idim, &h[(size_t)t * m.ldim], ti.data(), ta.data());
    for (int l = 0; l < m.layers; ++l) {
        const Layer &ly = m.L[l];
        for (int t = 0; t < T; ++t)
            matvec(ly.pw, nullptr, &h[(size_t)t * m.ldim], &p[(size_t)t * m.pdim], m.pdim, m.ldim);
        for (int t = 0; t < T; ++t) {
            float *o = &q[(size_t)t * m.pdim];
            const float *xc = &p[(size_t)t * m.pdim];
            for (int c = 0; c < m.pdim; ++c) o[c] = xc[c];
            for (int k = 0; k < m.lorder; ++k) {          // taps cover x[t-9 .. t]
                const int tt = t + k - (m.lorder - 1);
                if (tt < 0) continue;
                const float *xs = &p[(size_t)tt * m.pdim];
                for (int c = 0; c < m.pdim; ++c) o[c] += ly.cl[c * m.lorder + k] * xs[c];
            }
            for (int k = 0; k < m.rorder; ++k) {          // taps cover x[t+1 .. t+2]
                const int tt = t + 1 + k;
                if (tt >= T) continue;
                const float *xs = &p[(size_t)tt * m.pdim];
                for (int c = 0; c < m.pdim; ++c) o[c] += ly.cr[c * m.rorder + k] * xs[c];
            }
        }
        for (int t = 0; t < T; ++t) {
            matvec(ly.aw, ly.ab, &q[(size_t)t * m.pdim], &h[(size_t)t * m.ldim], m.ldim, m.pdim);
            relu(&h[(size_t)t * m.ldim], m.ldim);
        }
    }
    for (int t = 0; t < T; ++t)
        back(m, &h[(size_t)t * m.ldim], logits + (size_t)t * m.odim, ta.data());
}

void MemConv::init(int p, int lo, int ro) {
    pdim = p; lorder = lo; rorder = ro; ring = lo + ro;
    t = 0;
    buf.assign((size_t)ring * pdim, 0.f);
}

bool MemConv::push(const float *x, const Layer &ly, float *out) {
    memcpy(&buf[(size_t)(t % ring) * pdim], x, sizeof(float) * pdim);
    ++t;
    const long c = t - 1 - rorder;
    if (c < 0) return false;
    const float *xc = &buf[(size_t)(c % ring) * pdim];
    for (int i = 0; i < pdim; ++i) out[i] = xc[i];
    for (int k = 0; k < lorder; ++k) {
        const long tt = c + k - (lorder - 1);
        if (tt < 0) continue;
        const float *xs = &buf[(size_t)(tt % ring) * pdim];
        const float *wc = ly.cl + k;
        for (int i = 0; i < pdim; ++i) out[i] += wc[i * lorder] * xs[i];
    }
    for (int k = 0; k < rorder; ++k) {
        const float *xs = &buf[(size_t)((c + 1 + k) % ring) * pdim];
        const float *wc = ly.cr + k;
        for (int i = 0; i < pdim; ++i) out[i] += wc[i * rorder] * xs[i];
    }
    return true;
}

void Stream::init(const Model &mm) {
    m = &mm;
    finished = false;
    st.resize(m->layers);
    for (int i = 0; i < m->layers; ++i) st[i].init(m->pdim, m->lorder, m->rorder);
    ti.resize(m->idim);
    ta.resize(m->a1 > m->a2 ? m->a1 : m->a2);
    h.resize(m->ldim);
    p.resize(m->pdim);
    o.resize(m->pdim);
}

bool Stream::push(const float *feat, float *logits) {
    if (finished) return false;
    front(*m, feat, h.data(), ti.data(), ta.data());
    return advance(0, logits);
}

bool Stream::advance(int first_layer, float *logits) {
    for (int l = first_layer; l < m->layers; ++l) {
        matvec(m->L[l].pw, nullptr, h.data(), p.data(), m->pdim, m->ldim);
        if (!st[l].push(p.data(), m->L[l], o.data())) return false;
        matvec(m->L[l].aw, m->L[l].ab, o.data(), h.data(), m->ldim, m->pdim);
        relu(h.data(), m->ldim);
    }
    back(*m, h.data(), logits, ta.data());
    return true;
}

void Stream::finish(std::vector<float> &logits) {
    if (finished) return;
    finished = true;
    std::vector<float> zero(m->pdim, 0.0f), frame(m->odim);
    for (int l = 0; l < m->layers; ++l) {
        const long real_frames = st[l].t;
        if (real_frames == 0) continue;
        for (int k = 0; k < m->rorder; ++k) {
            if (!st[l].push(zero.data(), m->L[l], o.data())) continue;
            matvec(m->L[l].aw, m->L[l].ab, o.data(), h.data(), m->ldim, m->pdim);
            relu(h.data(), m->ldim);
            if (advance(l + 1, frame.data()))
                logits.insert(logits.end(), frame.begin(), frame.end());
        }
    }
}

}  // namespace kws::cfsmn
