/* -*- Mode: C++; -*- */
#ifndef __CHASE_GENERA_MATRIXFREE_CHASE__
#define __CHASE_GENERA_MATRIXFREE_CHASE__

#include <cstring>  //memcpy
#include <iostream>
#include <memory>
#include <random>

#ifdef HAS_MPI
#include "mpi.h"
#endif

#include "algorithm/chase.h"

#include "./blas_templates.h"
#include "./matrixfree_data.h"
//#include "./matrixfree_interface.h"
#include "genera/matrixfree/impl/mpi/matrixFreeMPI.hpp"

// TODO:
// -- random vectors for lanczos?

namespace chase {
namespace matrixfree {

template <template <typename> class MF, class T>
class MatrixFreeChase : public chase::Chase<T> {
 public:
  // case 1:
  // todo? take all arguments of matrices and entirely wrap it?
  MatrixFreeChase(std::size_t N, std::size_t nev, std::size_t nex,
                  T *V1 = nullptr, Base<T> *ritzv = nullptr, T *H = nullptr,
                  T *V2 = nullptr, Base<T> *resid = nullptr)
      : N_(N),
        nev_(nev),
        nex_(nex),
        locked_(0),
        config_(N, nev, nex),
        matrices_(N_, nev_ + nex_, V1, ritzv, H, V2, resid),
        gemm_(new MF<T>(matrices_, N_, nev_ + nex_)) {
    ritzv_ = matrices_.get_Ritzv();
    resid_ = matrices_.get_Resid();

    V_ = matrices_.get_V1();
    W_ = matrices_.get_V2();

    approxV_ = matrices_.get_V1();
    workspace_ = matrices_.get_V2();
    H_ = gemm_->get_H();
  }

  // case 2: MPI
  MatrixFreeChase(SkewedMatrixProperties<T> *properties, std::size_t N,
                  std::size_t nev, std::size_t nex, T *V1 = nullptr,
                  Base<T> *ritzv = nullptr, T *V2 = nullptr,
                  Base<T> *resid = nullptr)
      : N_(N),
        nev_(nev),
        nex_(nex),
        locked_(0),
        config_(N, nev, nex),
        properties_(properties),
        matrices_(std::move(
            properties_.get()->create_matrices(V1, ritzv, V2, resid))),
        gemm_(new MatrixFreeMPI<T>(properties_.get(),
                                   new MF<T>(properties_.get()))) {
    V_ = matrices_.get_V1();
    W_ = matrices_.get_V2();
    ritzv_ = matrices_.get_Ritzv();
    resid_ = matrices_.get_Resid();

    approxV_ = V_;
    workspace_ = W_;

    // H_ = gemm_->get_H();
    static_assert(is_skewed_matrixfree<MF<T>>::value,
                  "MatrixFreeChASE Must be skewed");
  }

  /*
// case 2:
MatrixFreeChase(config, skewed_matrices, comm )
{
static_assert(is_skewed_matrixfree<MF<T>>::value,"MatrixFreeChASE Must be
skewed"); auto properties = std::shared_ptr<SkewedMatrixProperties<T>>( new
SkewedMatrixProperties<T>(N, max_block, comm));

  auto matrices = ChASE_Blas_Matrices<T>(N, max_block, V, ritzv, properties);

  auto gemm_skewed = std::unique_ptr<MatrixFreeInterface<T>>(
      new MF<T>(properties));

  auto gemm = std::unique_ptr<MatrixFreeInterface<T>>(
      new MatrixFreeMPI<T>(properties, std::move(gemm_skewed)));
....
}
  */

  MatrixFreeChase(ChaseConfig<T> &config,
                  std::unique_ptr<MatrixFreeInterface<T>> gemm,
                  ChASE_Blas_Matrices<T> matrices)
      : N_(config.getN()),
        nev_(config.getNev()),
        nex_(config.getNex()),
        locked_(0),
        config_(config),
        gemm_(std::move(gemm)),
        matrices_(std::move(matrices)) {
    V_ = matrices_.get_V1();
    W_ = matrices_.get_V2();
    ritzv_ = matrices_.get_Ritzv();
    resid_ = matrices_.get_Resid();

    approxV_ = V_;
    workspace_ = W_;

    H_ = gemm_->get_H();
  }

  MatrixFreeChase(const MatrixFreeChase &) = delete;

  ~MatrixFreeChase() {}

  ChasePerfData GetPerfData() { return perf_; }

  ChaseConfig<T> &GetConfig() { return config_; }

  std::size_t GetN() const override { return N_; }

  CHASE_INT GetNev() override { return nev_; }

  CHASE_INT GetNex() override { return nex_; }

  Base<T> *GetRitzv() override { return ritzv_; }

  void Reset() { locked_ = 0; }

  T *GetVectorsPtr() { return approxV_; }

  T *GetWorkspacePtr() { return workspace_; }

  void Shift(T c, bool isunshift = false) override {
    if (!isunshift) {
      gemm_->preApplication(approxV_, locked_, nev_ + nex_ - locked_);
    }

    // for (std::size_t i = 0; i < N_; ++i) {
    //   H_[i + i * N_] += c;
    // }

    gemm_->shiftMatrix(c);
  };

  // todo this is wrong we want the END of V
  void Cpy(CHASE_INT new_converged){
      //    memcpy( workspace+locked*N, approxV+locked*N,
      //    N*(new_converged)*sizeof(T) );
      // memcpy(approxV + locked * N, workspace + locked * N,
      //     N * (new_converged) * sizeof(T));
  };

  void HEMM(CHASE_INT block, T alpha, T beta, CHASE_INT offset) override {
    // t_gemm(CblasColMajor, CblasNoTrans, CblasNoTrans,  //
    //        N_, block, N_,                              //
    //        &alpha,                                     //
    //        H_, N_,                                     //
    //        approxV_ + (locked_ + offset) * N_, N_,     //
    //        &beta,                                      //
    //        workspace_ + (locked_ + offset) * N_, N_);

    gemm_->apply(alpha, beta, offset, block);
    std::swap(approxV_, workspace_);
  };

  void Hv(T alpha);

  void QR(CHASE_INT fixednev) override {
    gemm_->postApplication(approxV_, nev_ + nex_ - locked_);

    std::size_t nevex = nev_ + nex_;
    T *tau = workspace_ + fixednev * N_;

    // we don't need this, as we copy to workspace when locking
    // std::memcpy(workspace_, approxV_, N_ * fixednev * sizeof(T));

    t_geqrf(LAPACK_COL_MAJOR, N_, nevex, approxV_, N_, tau);
    t_gqr(LAPACK_COL_MAJOR, N_, nevex, nevex, approxV_, N_, tau);

    std::memcpy(approxV_, workspace_, N_ * fixednev * sizeof(T));
  };

  void RR(Base<T> *ritzv, CHASE_INT block) override {
    // CHASE_INT block = nev+nex - fixednev;

    T *A = new T[block * block];  // For LAPACK.

    T One = T(1.0);
    T Zero = T(0.0);

    gemm_->preApplication(approxV_, locked_, block);
    gemm_->apply(One, Zero, 0, block);
    gemm_->postApplication(workspace_, block);

    // W <- H*V
    // t_gemm(CblasColMajor, CblasNoTrans, CblasNoTrans,  //
    //        N_, block, N_,                              //
    //        &One,                                       //
    //        H_, N_,                                     //
    //        approxV_ + locked_ * N_, N_,                //
    //        &Zero,                                      //
    //        workspace_ + locked_ * N_, N_);

    // A <- W' * V
    t_gemm(CblasColMajor, CblasConjTrans, CblasNoTrans,  //
           block, block, N_,                             //
           &One,                                         //
           approxV_ + locked_ * N_, N_,                  //
           workspace_ + locked_ * N_, N_,                //
           &Zero,                                        //
           A, block                                      //
    );

    t_heevd(LAPACK_COL_MAJOR, 'V', 'L', block, A, block, ritzv);

    t_gemm(CblasColMajor, CblasNoTrans, CblasNoTrans,  //
           N_, block, block,                           //
           &One,                                       //
           approxV_ + locked_ * N_, N_,                //
           A, block,                                   //
           &Zero,                                      //
           workspace_ + locked_ * N_, N_               //
    );

    std::swap(approxV_, workspace_);
    // we can swap, since the locked part were copied over as part of the QR

    delete[] A;
  };

  void Resd(Base<T> *ritzv, Base<T> *resid, CHASE_INT fixednev) override {
    T alpha = T(1.0);
    T beta = T(0.0);
    CHASE_INT unconverged = (nev_ + nex_) - fixednev;

    gemm_->preApplication(approxV_, locked_, unconverged);
    gemm_->apply(alpha, beta, 0, unconverged);
    gemm_->postApplication(workspace_, unconverged);

    // t_gemm(CblasColMajor, CblasNoTrans, CblasNoTrans,  //
    //        N_, unconverged, N_,                        //
    //        &alpha,                                     //
    //        H_, N_,                                     //
    //        approxV_ + locked_ * N_, N_,                //
    //        &beta,                                      //
    //        workspace_ + locked_ * N_, N_);

    for (std::size_t i = 0; i < unconverged; ++i) {
      beta = T(-ritzv[i]);
      t_axpy(                                      //
          N_,                                      //
          &beta,                                   //
          (approxV_ + locked_ * N_) + N_ * i, 1,   //
          (workspace_ + locked_ * N_) + N_ * i, 1  //
      );

      resid[i] = t_nrm2(N_, (workspace_ + locked_ * N_) + N_ * i, 1);
    }
  };

  void Swap(CHASE_INT i, CHASE_INT j) override {
    T *ztmp = new T[N_];
    memcpy(ztmp, approxV_ + N_ * i, N_ * sizeof(T));
    memcpy(approxV_ + N_ * i, approxV_ + N_ * j, N_ * sizeof(T));
    memcpy(approxV_ + N_ * j, ztmp, N_ * sizeof(T));

    memcpy(ztmp, workspace_ + N_ * i, N_ * sizeof(T));
    memcpy(workspace_ + N_ * i, workspace_ + N_ * j, N_ * sizeof(T));
    memcpy(workspace_ + N_ * j, ztmp, N_ * sizeof(T));
    delete[] ztmp;
  };

  void Lanczos(CHASE_INT m, Base<T> *upperb) override {
    // todo
    CHASE_INT n = N_;

    T *v1 = workspace_;
    // std::random_device rd;
    std::mt19937 gen(2342.0);
    std::normal_distribution<> normal_distribution;

    for (std::size_t k = 0; k < N_; ++k)
      v1[k] = getRandomT<T>([&]() { return normal_distribution(gen); });

    // assert( m >= 1 );
    Base<T> *d = new Base<T>[m]();
    Base<T> *e = new Base<T>[m]();

    // SO C++03 5.3.4[expr.new]/15
    T *v0_ = new T[n]();
    T *w_ = new T[n]();

    T *v0 = v0_;
    T *w = w_;

    T alpha = T(1.0);
    T beta = T(0.0);
    T One = T(1.0);
    T Zero = T(0.0);

    //  T *v1 = V;
    // ENSURE that v1 has one norm
    Base<T> real_alpha = t_nrm2(n, v1, 1);
    alpha = T(1 / real_alpha);
    t_scal(n, &alpha, v1, 1);
    Base<T> real_beta = 0;

    real_beta = 0;

    for (std::size_t k = 0; k < m; ++k) {
      // t_gemv(CblasColMajor, CblasNoTrans, N_, N_, &One, H_, N_, v1, 1, &Zero,
      // w, 1);

      gemm_->applyVec(v1, w);

      alpha = t_dot(n, v1, 1, w, 1);

      alpha = -alpha;
      t_axpy(n, &alpha, v1, 1, w, 1);
      alpha = -alpha;

      d[k] = std::real(alpha);
      if (k == m - 1) break;

      beta = T(-real_beta);
      t_axpy(n, &beta, v0, 1, w, 1);
      beta = -beta;

      real_beta = t_nrm2(n, w, 1);
      beta = T(1.0 / real_beta);

      t_scal(n, &beta, w, 1);

      e[k] = real_beta;

      std::swap(v1, v0);
      std::swap(v1, w);
    }

    delete[] w_;
    delete[] v0_;

    CHASE_INT notneeded_m;
    CHASE_INT vl, vu;
    Base<T> ul, ll;
    CHASE_INT tryrac = 0;
    CHASE_INT *isuppz = new CHASE_INT[2 * m];
    Base<T> *ritzv = new Base<T>[m];

    t_stemr<Base<T>>(LAPACK_COL_MAJOR, 'N', 'A', m, d, e, ul, ll, vl, vu,
                     &notneeded_m, ritzv, NULL, m, m, isuppz, &tryrac);

    *upperb = std::max(std::abs(ritzv[0]), std::abs(ritzv[m - 1])) +
              std::abs(real_beta);

    delete[] ritzv;
    delete[] isuppz;
    delete[] d;
    delete[] e;
  };

  // we need to be careful how we deal with memory here
  // we will operate within Workspace
  void Lanczos(CHASE_INT M, CHASE_INT idx, Base<T> *upperb, Base<T> *ritzv,
               Base<T> *Tau, Base<T> *ritzV) override {
    // todo
    CHASE_INT m = M;
    CHASE_INT n = N_;

    // assert( m >= 1 );

    // The first m*N part is reserved for the lanczos vectors
    Base<T> *d = new Base<T>[m]();
    Base<T> *e = new Base<T>[m]();

    // SO C++03 5.3.4[expr.new]/15
    T *v0_ = new T[n]();
    T *w_ = new T[n]();

    T *v0 = v0_;
    T *w = w_;

    T alpha = T(1.0);
    T beta = T(0.0);
    T One = T(1.0);
    T Zero = T(0.0);

    // V is filled with randomness
    T *v1 = workspace_;
    for (std::size_t k = 0; k < N_; ++k) v1[k] = V_[k + idx * N_];

    // ENSURE that v1 has one norm
    Base<T> real_alpha = t_nrm2(n, v1, 1);
    alpha = T(1 / real_alpha);
    t_scal(n, &alpha, v1, 1);

    Base<T> real_beta = 0.0;

    for (std::size_t k = 0; k < m; ++k) {
      if (workspace_ + k * n != v1)
        memcpy(workspace_ + k * n, v1, n * sizeof(T));

      // t_gemv(CblasColMajor, CblasNoTrans, n, n, &One, H_, n, v1, 1, &Zero, w,
      // 1);

      gemm_->applyVec(v1, w);

      // std::cout << "lanczos Av\n";
      // for (std::size_t ll = 0; ll < 2; ++ll)
      //   std::cout << w[ll] << "\n";

      alpha = t_dot(n, v1, 1, w, 1);

      alpha = -alpha;
      t_axpy(n, &alpha, v1, 1, w, 1);
      alpha = -alpha;

      d[k] = std::real(alpha);
      if (k == m - 1) break;

      beta = T(-real_beta);
      t_axpy(n, &beta, v0, 1, w, 1);
      beta = -beta;

      real_beta = t_nrm2(n, w, 1);
      beta = T(1.0 / real_beta);

      t_scal(n, &beta, w, 1);

      e[k] = real_beta;

      std::swap(v1, v0);
      std::swap(v1, w);
    }

    delete[] w_;
    delete[] v0_;

    CHASE_INT notneeded_m;
    CHASE_INT vl, vu;
    Base<T> ul, ll;
    CHASE_INT tryrac = 0;
    CHASE_INT *isuppz = new CHASE_INT[2 * m];

    t_stemr(LAPACK_COL_MAJOR, 'V', 'A', m, d, e, ul, ll, vl, vu, &notneeded_m,
            ritzv, ritzV, m, m, isuppz, &tryrac);

    *upperb = std::max(std::abs(ritzv[0]), std::abs(ritzv[m - 1])) +
              std::abs(real_beta);

    for (std::size_t k = 1; k < m; ++k) {
      Tau[k] = std::abs(ritzV[k * m]) * std::abs(ritzV[k * m]);
      // std::cout << Tau[k] << "\n";
    }

    delete[] isuppz;
    delete[] d;
    delete[] e;
  };

  void Lock(CHASE_INT new_converged) override {
    std::memcpy(workspace_ + locked_ * N_, approxV_ + locked_ * N_,
                N_ * (new_converged) * sizeof(T));
    locked_ += new_converged;
  };
  /*
  double compare(T *V_) {
    double norm = 0;
    for (CHASE_INT i = 0; i < (nev_ + nex_) * N; ++i)
      norm += std::abs(V_[i] - approxV[i]) * std::abs(V_[i] - approxV[i]);
    std::cout << "norm: " << norm << "\n";

    norm = 0;
    for (CHASE_INT i = 0; i < (locked_)*N; ++i)
      norm += std::abs(V_[i] - approxV[i]) * std::abs(V_[i] - approxV[i]);
    std::cout << "norm: " << norm << "\n";
  }
  */
  void LanczosDos(CHASE_INT idx, CHASE_INT m, T *ritzVc) override {
    T alpha = T(1.0);
    T beta = T(0.0);

    t_gemm(CblasColMajor, CblasNoTrans, CblasNoTrans,  //
           N_, idx, m,                                 //
           &alpha,                                     //
           workspace_, N_,                             //
           ritzVc, m,                                  //
           &beta,                                      //
           approxV_, N_                                //
    );
  }

  Base<T> Residual() {
    for (CHASE_INT j = 0; j < N_ * (nev_ + nex_); ++j) {
      workspace_[j] = approxV_[j];
    }

    //    memcpy(W, V, sizeof(MKL_Complex16)*N*nev);
    T one(1.0);
    T zero(0.0);
    T eigval;
    int iOne = 1;
    for (int ttz = 0; ttz < nev_; ttz++) {
      eigval = -1.0 * ritzv_[ttz];
      t_scal(N_, &eigval, workspace_ + ttz * N_, 1);
    }

    gemm_->preApplication(approxV_, workspace_, 0, nev_);
    gemm_->apply(one, one, 0, nev_);
    gemm_->postApplication(workspace_, nev_);

    // t_hemm(CblasColMajor, CblasLeft, CblasLower,  //
    //        N_, nev_,                              //
    //        &one,                                  //
    //        H_, N_,                                //
    //        V_, N_,                                //
    //        &one, W_, N_);

    Base<T> norm = t_lange('M', N_, nev_, workspace_, N_);
    return norm;
  }

  Base<T> Orthogonality() {
    T one(1.0);
    T zero(0.0);
    // Check eigenvector orthogonality
    auto unity = std::unique_ptr<T[]>(new T[nev_ * nev_]);
    T neg_one(-1.0);
    for (int ttz = 0; ttz < nev_; ttz++) {
      for (int tty = 0; tty < nev_; tty++) {
        if (ttz == tty)
          unity[nev_ * ttz + tty] = 1.0;
        else
          unity[nev_ * ttz + tty] = 0.0;
      }
    }

    t_gemm(CblasColMajor, CblasConjTrans, CblasNoTrans, nev_, nev_, N_, &one,
           &*approxV_, N_, &*approxV_, N_, &neg_one, &unity[0], nev_);
    Base<T> norm = t_lange('M', nev_, nev_, &unity[0], nev_);
    return norm;
  }

  void Output(std::string str) override {
    int rank = 0;
#ifdef HAS_MPI
    int init = 0;
    MPI_Initialized(&init);
    if (init) MPI_Comm_rank(MPI_COMM_WORLD, &rank);
#endif
    if (rank == 0) std::cout << str;
  }

  T *GetMatrixPtr() { return gemm_->get_H(); }

  void GetOff(CHASE_INT *xoff, CHASE_INT *yoff, CHASE_INT *xlen,
              CHASE_INT *ylen) {
    gemm_->get_off(xoff, yoff, xlen, ylen);
  }

  Base<T> *GetResid() { return resid_; }

 private:
  const std::size_t N_;
  const std::size_t nev_;
  const std::size_t nex_;
  std::size_t locked_;

  T *H_;
  T *V_;
  T *W_;
  T *approxV_;
  T *workspace_;

  Base<T> *ritzv_;
  Base<T> *resid_;

  std::unique_ptr<SkewedMatrixProperties<T>> properties_;
  ChASE_Blas_Matrices<T> matrices_;
  std::unique_ptr<MatrixFreeInterface<T>> gemm_;

  ChaseConfig<T> config_;
  ChasePerfData perf_;
};

// TODO
/*
void check_params(std::size_t N, std::size_t nev, std::size_t nex,
                  const double tol, std::size_t deg )
{
  bool abort_flag = false;
  if(tol < 1e-14)
    std::clog << "WARNING: Tolerance too small, may take a while." << std::endl;
  if(deg < 8 || deg > ChASE_Config::chase_max_deg)
    std::clog << "WARNING: Degree should be between 8 and " <<
ChASE_Config::chase_max_deg << "."
              << " (current: " << deg << ")" << std::endl;
  if((double)nex/nev < 0.15 || (double)nex/nev > 0.75)
    {
      std::clog << "WARNING: NEX should be between 0.15*NEV and 0.75*NEV."
                << " (current: " << (double)nex/nev << ")" << std::endl;
      //abort_flag = true;
    }
  if(nev+nex > N)
    {
      std::cerr << "ERROR: NEV+NEX has to be smaller than N." << std::endl;
      abort_flag = true;
    }

  if(abort_flag)
    {
      std::cerr << "Stopping execution." << std::endl;
      exit(-1);
    }
 }
*/
}  // namespace matrixfree
}  // namespace chase
#endif
