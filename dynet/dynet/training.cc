#include "dynet/training.h"

#include <boost/serialization/vector.hpp>
#include <boost/serialization/export.hpp>

// #include "dynet/gpu-ops.h"
#include "dynet/param-nodes.h"
#include "dynet/weight-decay.h"
#include "dynet/io-macros.h"

// Macros for defining parameter update functions
#ifdef __CUDACC__
#define DYNET_TRAINER_INST_DEV_IMPL(MyTrainer) \
  template void MyTrainer::update_rule_dev<Device_GPU>(const Device_GPU & dev, real scale, real gscale, const std::vector<Tensor*> & values);
#elif defined(HAVE_CUDA)
// This is correct, but dying when models are read and written.
// if(values[0]->device->type == DeviceType::CPU) { update_rule_dev(*(Device_CPU*)values[0]->device,scale,gscale,values); } 
// else if(values[0]->device->type == DeviceType::GPU) { update_rule_dev(*(Device_GPU*)values[0]->device,scale,gscale,values); } 
// else { abort(); }
#define DYNET_TRAINER_INST_DEV_IMPL(MyTrainer) \
  extern template void MyTrainer::update_rule_dev<Device_GPU>(const Device_GPU & dev, real scale, real gscale, const std::vector<Tensor*> & values); \
  template void MyTrainer::update_rule_dev<Device_CPU>(const Device_CPU & dev, real scale, real gscale, const std::vector<Tensor*> & values); \
  void MyTrainer::update_rule(real scale, real gscale, const std::vector<Tensor*> & values) { \
    if(default_device->type == DeviceType::CPU) { update_rule_dev(*(Device_CPU*)default_device,scale,gscale,values); } \
    else if(default_device->type == DeviceType::GPU) { update_rule_dev(*(Device_GPU*)default_device,scale,gscale,values); } \
    else { abort(); } \
  }
#else
#define DYNET_TRAINER_INST_DEV_IMPL(MyTrainer) \
  template void MyTrainer::update_rule_dev<Device_CPU>(const Device_CPU & dev, real scale, real gscale, const std::vector<Tensor*> & values); \
  void MyTrainer::update_rule(real scale, real gscale, const std::vector<Tensor*> & values) { \
    if(default_device->type == DeviceType::CPU) { update_rule_dev(*(Device_CPU*)default_device,scale,gscale,values); } \
    else { abort(); } \
  }
#endif

namespace dynet {

using namespace std;

template <class Derived>
bool is_valid(const Eigen::MatrixBase<Derived>& x) {
  return ((x - x).array() == (x - x).array()).all();
}

// --- The actual update code for each operation, implemented on various devices

// Trainer base class is run on CPUs
#ifndef __CUDACC__

Trainer::~Trainer() {}

void Trainer::rescale_and_reset_weight_decay() {
  const float weight_decay = model->weight_decay.current_weight_decay();
  const auto params = model->parameters_list();
  for (auto p : model->updated_parameters_list())
    params[p]->scale_parameters(weight_decay);
  const auto lookup_params = model->parameters_list();
  for (auto p : model->updated_lookup_parameters_list())
    lookup_params[p]->scale_parameters(weight_decay);
  model->weight_decay.reset_weight_decay();
}

float Trainer::clip_gradients(real scale) {
  float gscale = 1;
  if (clipping_enabled) {
    // TODO should I handle updatebale differently?
    float gg = model->gradient_l2_norm();
    if (isnan(gg) || isinf(gg)) {
      cerr << "Magnitude of gradient is bad: " << gg << endl;
      abort();
    }
    if (scale * gg > clip_threshold) {
      ++clips;
      ++clips_since_status;
      gscale = clip_threshold / (scale * gg);
    }
  }
  return gscale;
}

// this calls the rule-specific
void Trainer::update(real scale) {
  // Allocate if necessary
  if(!aux_allocated) {
    alloc_impl();
    aux_allocated = true;
  }

  // Perform gradient clipping and cycle through parameters
  const float gscale = clip_gradients(scale);
  const auto & params = model->parameters_list();
  const auto & upd_params = model->updated_parameters_list();
  for(auto i : upd_params) {
    update_params(scale, gscale, i);
    params[i]->clear();
  }
  const auto & lookup_params = model->lookup_parameters_list();
  const auto & upd_lookup_params = model->updated_lookup_parameters_list();
  for(auto i : upd_lookup_params) {
    if(sparse_updates_enabled) {
      for (auto j : lookup_params[i]->non_zero_grads)
        update_lookup_params(scale, gscale, i, j);
    } else {
      update_lookup_params(scale, gscale, i);
    }
    lookup_params[i]->clear();
  }
  ++updates;
  ++updates_since_status;

  model->weight_decay.update_weight_decay(); // update global weight scale
  if (model->weight_decay.parameters_need_rescaled())
    rescale_and_reset_weight_decay();  // if wdscale is getting to small multiply all weights by wdscale, and set wdscale to 1
}

#endif

// --- SimpleSGDTrainer

// Perform update of ts[0]=parameters, ts[1]=gradients
template <class MyDevice>
void SimpleSGDTrainer::update_rule_dev(const MyDevice & dev, real scale, real gscale, const std::vector<Tensor*> & ts) {
  ts[0]->tvec().device(*dev.edevice) -= ts[1]->tvec() * (eta * scale * gscale / model->weight_decay.current_weight_decay());
}
DYNET_TRAINER_INST_DEV_IMPL(SimpleSGDTrainer)

#ifndef __CUDACC__
void SimpleSGDTrainer::update_params(real scale, real gscale, size_t idx) {
  auto & p = model->parameters_list()[idx];
  update_rule(scale, gscale, {&p->values, &p->g});
}
void SimpleSGDTrainer::update_lookup_params(real scale, real gscale, size_t idx, size_t lidx) {
  auto & p = model->lookup_parameters_list()[idx];
  update_rule(scale, gscale, {&p->values[lidx], &p->grads[lidx]});
}
void SimpleSGDTrainer::update_lookup_params(real scale, real gscale, size_t idx) {
  auto & p = model->lookup_parameters_list()[idx];
  update_rule(scale, gscale, {&p->all_values, &p->all_grads});
}
#endif

// --- MomentumSGDTrainer

// Perform update of ts[0]=parameters, ts[1]=gradients, ts[2]=momentum
template <class MyDevice>
void MomentumSGDTrainer::update_rule_dev(const MyDevice & dev, real scale, real gscale, const std::vector<Tensor*> & ts) {
  ts[2]->tvec().device(*dev.edevice) = ts[2]->tvec() * momentum - ts[1]->tvec() * (eta * scale * gscale);
  ts[0]->tvec().device(*dev.edevice) += ts[2]->tvec() / model->weight_decay.current_weight_decay();
}
DYNET_TRAINER_INST_DEV_IMPL(MomentumSGDTrainer)

#ifndef __CUDACC__
void MomentumSGDTrainer::update_params(real scale, real gscale, size_t idx) {
  auto & p = model->parameters_list()[idx];
  update_rule(scale, gscale, {&p->values, &p->g, &vp[idx].h});
}
void MomentumSGDTrainer::update_lookup_params(real scale, real gscale, size_t idx, size_t lidx) {
  auto & p = model->lookup_parameters_list()[idx];
  update_rule(scale, gscale, {&p->values[lidx], &p->grads[lidx], &vlp[idx].h[lidx]});
}
void MomentumSGDTrainer::update_lookup_params(real scale, real gscale, size_t idx) {
  auto & p = model->lookup_parameters_list()[idx];
  update_rule(scale, gscale, {&p->all_values, &p->all_grads, &vlp[idx].all_h});
}
void MomentumSGDTrainer::alloc_impl() {
  vp = allocate_shadow_parameters(*model);
  vlp = allocate_shadow_lookup_parameters(*model);
}
#endif

// --- AdagradTrainer

// Perform update of ts[0]=parameters, ts[1]=gradients, ts[2]=stddev
template <class MyDevice>
void AdagradTrainer::update_rule_dev(const MyDevice & dev, real scale, real gscale, const std::vector<Tensor*> & ts) {
  ts[1]->tvec().device(*dev.edevice) = ts[1]->tvec() * (scale * gscale);
  ts[2]->tvec().device(*dev.edevice) += ts[1]->tvec().square();
  ts[0]->tvec().device(*dev.edevice) += ts[1]->tvec() / (ts[2]->tvec() + epsilon).sqrt() * (-eta / model->weight_decay.current_weight_decay());
}
DYNET_TRAINER_INST_DEV_IMPL(AdagradTrainer)

#ifndef __CUDACC__
void AdagradTrainer::update_params(real scale, real gscale, size_t idx) {
  auto & p = model->parameters_list()[idx];
  update_rule(scale, gscale, {&p->values, &p->g, &vp[idx].h});
}
void AdagradTrainer::update_lookup_params(real scale, real gscale, size_t idx, size_t lidx) {
  auto & p = model->lookup_parameters_list()[idx];
  update_rule(scale, gscale, {&p->values[lidx], &p->grads[lidx], &vlp[idx].h[lidx]});
}
void AdagradTrainer::update_lookup_params(real scale, real gscale, size_t idx) {
  auto & p = model->lookup_parameters_list()[idx];
  update_rule(scale, gscale, {&p->all_values, &p->all_grads, &vlp[idx].all_h});
}
void AdagradTrainer::alloc_impl() {
  vp = allocate_shadow_parameters(*model);
  vlp = allocate_shadow_lookup_parameters(*model);
}
#endif

// --- AdadeltaTrainer

// Perform update of ts[0]=parameters, ts[1]=gradients, ts[2]=hg, ts[3]=hd
template <class MyDevice>
void AdadeltaTrainer::update_rule_dev(const MyDevice & dev, real scale, real gscale, const std::vector<Tensor*> & ts) {
  ts[1]->tvec().device(*dev.edevice) = ts[1]->tvec() * (scale * gscale);
  ts[2]->tvec().device(*dev.edevice) = ts[2]->tvec() * rho + ts[1]->tvec().square() * (1.f - rho);
  ts[1]->tvec().device(*dev.edevice) = - ts[1]->tvec() * (ts[3]->tvec() + epsilon).sqrt() / (ts[2]->tvec() + epsilon).sqrt();
  ts[3]->tvec().device(*dev.edevice) = ts[3]->tvec() * rho + ts[1]->tvec().square() * (1.f - rho);
  ts[0]->tvec().device(*dev.edevice) += ts[1]->tvec() / model->weight_decay.current_weight_decay();
}
DYNET_TRAINER_INST_DEV_IMPL(AdadeltaTrainer)

#ifndef __CUDACC__
void AdadeltaTrainer::update_params(real scale, real gscale, size_t idx) {
  auto & p = model->parameters_list()[idx];
  update_rule(scale, gscale, {&p->values, &p->g, &hg[idx].h, &hd[idx].h});
}
void AdadeltaTrainer::update_lookup_params(real scale, real gscale, size_t idx, size_t lidx) {
  auto & p = model->lookup_parameters_list()[idx];
  update_rule(scale, gscale, {&p->values[lidx], &p->grads[lidx], &hlg[idx].h[lidx], &hld[idx].h[lidx]});
}
void AdadeltaTrainer::update_lookup_params(real scale, real gscale, size_t idx) {
  auto & p = model->lookup_parameters_list()[idx];
  update_rule(scale, gscale, {&p->all_values, &p->all_grads, &hlg[idx].all_h, &hld[idx].all_h});
}
void AdadeltaTrainer::alloc_impl() {
  hg = allocate_shadow_parameters(*model);
  hlg = allocate_shadow_lookup_parameters(*model);
  hd = allocate_shadow_parameters(*model);
  hld = allocate_shadow_lookup_parameters(*model);
}
#endif

// --- RmsPropTrainer
// TODO: This is not finished yet, because it memorizes a scalar for each set of parameters, not each parameter itself.
//       We could implement this with one tensor for each scalar, but this is pretty wasteful

// Perform update of ts[0]=parameters, ts[1]=gradients
template <class MyDevice>
void RmsPropTrainer::update_rule_dev(const MyDevice & dev, real scale, real gscale, const std::vector<Tensor*> & ts) {
  throw std::runtime_error("RMSProp optimization not implemented yet.");
  // real& d2 = hg[pi++];
  // real g2 = p->g.vec().squaredNorm();
  // d2 = rho * d2 + (1.f - rho) * g2;
  // p->values.vec() -= ((eta * scale * gscale / sqrt(d2 + epsilon)) * p->g.vec()) / model->weight_decay.current_weight_decay();
}
DYNET_TRAINER_INST_DEV_IMPL(RmsPropTrainer)

#ifndef __CUDACC__
void RmsPropTrainer::update_params(real scale, real gscale, size_t idx) {
  throw std::runtime_error("RMSProp optimization not implemented yet.");
  // auto & p = model->parameters_list()[idx];
  // update_rule(scale, gscale, {&p->values, &p->g, &hg[idx].h, &hd[idx].h});
}
void RmsPropTrainer::update_lookup_params(real scale, real gscale, size_t idx, size_t lidx) {
  throw std::runtime_error("RMSProp optimization not implemented yet.");
  // auto & p = model->lookup_parameters_list()[idx];
  // update_rule(scale, gscale, {&p->values[lidx], &p->grads[lidx], &hlg[idx].h[lidx], &hld[idx].h[lidx]});
}
void RmsPropTrainer::update_lookup_params(real scale, real gscale, size_t idx) {
  throw std::runtime_error("RMSProp optimization not implemented yet.");
  // auto & p = model->lookup_parameters_list()[idx];
  // update_rule(scale, gscale, {&p->all_values, &p->all_grads, &hlg[idx].all_h, &hld[idx].all_h});
}
void RmsPropTrainer::alloc_impl() {
  throw std::runtime_error("RMSProp optimization not implemented yet.");
  // hg.resize(model->parameters_list().size());
  // unsigned pi = 0;
  // hlg.resize(model->lookup_parameters_list().size());
  // for (auto p : model->lookup_parameters_list()) {
  //   hlg[pi++].resize(p->size());
  // }
}
#endif

// --- AdamTrainer

// Perform update of ts[0]=parameters, ts[1]=gradients, ts[2]=mean, ts[3]=variance
template <class MyDevice>
void AdamTrainer::update_rule_dev(const MyDevice & dev, real scale, real gscale, const std::vector<Tensor*> & ts) {
  ts[1]->tvec().device(*dev.edevice) = ts[1]->tvec() * (scale * gscale);
  ts[2]->tvec().device(*dev.edevice) = ts[2]->tvec() * beta_1 + ts[1]->tvec() * (1.f - beta_1);
  ts[3]->tvec().device(*dev.edevice) = ts[3]->tvec() * beta_2 + ts[1]->tvec().square() * (1.f - beta_2);
  float lr_t = eta * sqrt(1-pow(beta_2, updates+1))/(1-pow(beta_1, updates+1))/ model->weight_decay.current_weight_decay();
  ts[0]->tvec().device(*dev.edevice) -= ts[2]->tvec() / (ts[3]->tvec().sqrt() + epsilon) * lr_t;
}
DYNET_TRAINER_INST_DEV_IMPL(AdamTrainer)

#ifndef __CUDACC__
void AdamTrainer::update_params(real scale, real gscale, size_t idx) {
  auto & p = model->parameters_list()[idx];
  update_rule(scale, gscale, {&p->values, &p->g, &m[idx].h, &v[idx].h});
}
void AdamTrainer::update_lookup_params(real scale, real gscale, size_t idx, size_t lidx) {
  auto & p = model->lookup_parameters_list()[idx];
  update_rule(scale, gscale, {&p->values[lidx], &p->grads[lidx], &lm[idx].h[lidx], &lv[idx].h[lidx]});
}
void AdamTrainer::update_lookup_params(real scale, real gscale, size_t idx) {
  auto & p = model->lookup_parameters_list()[idx];
  update_rule(scale, gscale, {&p->all_values, &p->all_grads, &lm[idx].all_h, &lv[idx].all_h});
}
void AdamTrainer::alloc_impl() {
  m = allocate_shadow_parameters(*model);
  lm = allocate_shadow_lookup_parameters(*model);
  v = allocate_shadow_parameters(*model);
  lv = allocate_shadow_lookup_parameters(*model);
}
#endif

// --- AdaptiveEGTrainer

// Perform update of ts[0]=parameters, ts[1]=gradients, ts[2]=accumulated_gradients
template <class MyDevice>
void AdaptiveEGTrainer::update_rule_dev(const MyDevice & dev, real scale, real gscale, const std::vector<Tensor*> & ts) {
  // --------------------------
  // AdaGrad step
  /*real epsilon = 1e-8;
  ts[1]->tvec().device(*dev.edevice) = ts[1]->tvec() * (scale * gscale);
  ts[2]->tvec().device(*dev.edevice) += ts[1]->tvec().square();*/
  //ts[0]->tvec().device(*dev.edevice) += ts[1]->tvec() / (ts[2]->tvec() + epsilon).sqrt() * (-eta / model->weight_decay.current_weight_decay());
  // --------------------------

  // --------------------------
  // Adam step
  float beta_1 = 0.9, beta_2 = 0.999, eps = 1e-8;
  ts[1]->tvec().device(*dev.edevice) = ts[1]->tvec() * (scale * gscale);
  ts[2]->tvec().device(*dev.edevice) = ts[2]->tvec() * beta_1 + ts[1]->tvec() * (1.f - beta_1);
  ts[3]->tvec().device(*dev.edevice) = ts[3]->tvec() * beta_2 + ts[1]->tvec().square() * (1.f - beta_2);
  float s1 = 1 - pow(beta_1, updates+1);
  float s2 = 1 - pow(beta_2, updates+1);
  //ts[0]->tvec().device(*dev.edevice) += ts[2]->tvec() / ((ts[3]->tvec() / s2).sqrt() + epsilon) * (-eta / s1 / model->weight_decay.current_weight_decay());
  // --------------------------
  
  // --------------------------
  // For debug only
  //cerr << "eta=" << eta << "; " << "scale=" << scale << "; " << "gscale=" << gscale << "; " << "weight_decay=" << weight_decay << endl;
  std::vector<real> v_params = dynet::as_vector(*ts[0]);
  //std::vector<real> v_grads = dynet::as_vector(*ts[1]);
  std::vector<real> v_m1 = dynet::as_vector(*ts[2]);// for adam step
  std::vector<real> v_m2 = dynet::as_vector(*ts[3]);// for adam step
  //std::vector<real> v_acc_grads = dynet::as_vector(*ts[2]);// for AdaGrad step
  //cerr << "params[0]=" << v_params[0] << "; params[1]=" << v_params[1] << endl;
  //cerr << "grads[0]=" << v_grads[0] << "; grads[1]=" << v_grads[1] << endl;
  //cerr << "EG_v[0]=" << log(v_params[0] / weight_decay) - (eta * scale * gscale) * v_grads[0] << endl;
  //cerr << "EG_v[1]=" << log(v_params[1] / weight_decay) - (eta * scale * gscale) * v_grads[1] << endl;
  //cerr << "max(grads)=" << *std::max_element(v_grads.begin(), v_grads.end()) << endl;

  Eigen::TensorMap<Eigen::Tensor<real,1>> t_params(&v_params[0], v_params.size());
  //Eigen::TensorMap<Eigen::Tensor<real,1>> t_grads(&v_grads[0], v_grads.size());
  Eigen::TensorMap<Eigen::Tensor<real,1>> t_m1(&v_m1[0], v_m1.size());// for adam step
  Eigen::TensorMap<Eigen::Tensor<real,1>> t_m2(&v_m2[0], v_m2.size());// for adam step
  //Eigen::TensorMap<Eigen::Tensor<real,1>> t_acc_grads(&v_acc_grads[0], v_acc_grads.size());// for AdaGrad step
  //Eigen::Tensor<real,1> v = t_params.log() - t_grads * (eta / (t_acc_grads + epsilon).sqrt() / model->weight_decay.current_weight_decay());// for AdaGrad step
  Eigen::Tensor<real,1> v = t_params.log() - t_m1 / ((t_m2 / s2).sqrt() + eps) * (eta / s1 / model->weight_decay.current_weight_decay());// for adam step
  
  Eigen::Tensor<float,0> m = v.maximum() ;// max
  Eigen::Tensor<float,0> z = m.coeff() + (v - m.coeff()).exp().sum().log();// Z
  Eigen::Tensor<float,1> u = (v - z.coeff()).exp();// result
  TensorTools::SetElements(*ts[0], u.data(), u.dimensions()[0]);   
}
DYNET_TRAINER_INST_DEV_IMPL(AdaptiveEGTrainer)

#ifndef __CUDACC__
void AdaptiveEGTrainer::update_params(real scale, real gscale, size_t idx) {
  auto & p = model->parameters_list()[idx];
  //update_rule(scale, gscale, {&p->values, &p->g});
  //update_rule(scale, gscale, {&p->values, &p->g, &vp[idx].h});
  update_rule(scale, gscale, {&p->values, &p->g, &m[idx].h, &v[idx].h});
}
void AdaptiveEGTrainer::update_lookup_params(real scale, real gscale, size_t idx, size_t lidx) {
  auto & p = model->lookup_parameters_list()[idx];
  //update_rule(scale, gscale, {&p->values[lidx], &p->grads[lidx]});
  //update_rule(scale, gscale, {&p->values[lidx], &p->grads[lidx], &vlp[idx].h[lidx]});
  update_rule(scale, gscale, {&p->values[lidx], &p->grads[lidx], &lm[idx].h[lidx], &lv[idx].h[lidx]});
}
void AdaptiveEGTrainer::update_lookup_params(real scale, real gscale, size_t idx) {
  auto & p = model->lookup_parameters_list()[idx];
  //update_rule(scale, gscale, {&p->all_grads, &p->all_grads});
  //update_rule(scale, gscale, {&p->all_grads, &p->all_grads, &vlp[idx].all_h});
  update_rule(scale, gscale, {&p->all_grads, &p->all_grads, &lm[idx].all_h, &lv[idx].all_h});
}
void AdaptiveEGTrainer::alloc_impl() {
  //vp = allocate_shadow_parameters(*model);
  //vlp = allocate_shadow_lookup_parameters(*model);
  m = allocate_shadow_parameters(*model);
  lm = allocate_shadow_lookup_parameters(*model);
  v = allocate_shadow_parameters(*model);
  lv = allocate_shadow_lookup_parameters(*model);
  //v_etas.resize(model->all_parameters_list().size(), eta0);// initial learning rates
}
#endif

// --- EGTrainer
template <class MyDevice>
void EGTrainer::update_rule_dev(const MyDevice & dev, real scale, real gscale, const std::vector<Tensor*> & ts) {
  //------------------------------------------------------------------
  // Add white Gaussian noise
  //real stddev = scale;// FIXME: should be an *annealing* factor
  //TensorTools::RandomizeNormal(0, stddev, *ts[2]);
  //ts[1]->tvec().device(*dev.edevice) += ts[2]->tvec();

  //------------------------------------------------------------------
  // Add momentum
  //real dampening = momentum;
  ts[2]->tvec().device(*dev.edevice) = ts[2]->tvec() * momentum - ts[1]->tvec() * (eta * scale * gscale);
  //ts[0]->tvec().device(*dev.edevice) += ts[2]->tvec() / model->weight_decay.current_weight_decay();

  //------------------------------------------------------------------
  std::vector<real> v_params = dynet::as_vector(*ts[0]);// FIXME: not a smart way but it works on both GPU and CPU!
  //std::vector<real> v_grads = dynet::as_vector(*ts[1]);
  std::vector<real> v_grads = dynet::as_vector(*ts[2]);// gradients with momentum
  Eigen::TensorMap<Eigen::Tensor<real,1>> t_params(&v_params[0], v_params.size());
  Eigen::TensorMap<Eigen::Tensor<real,1>> t_grads(&v_grads[0], v_grads.size());
  //Eigen::Tensor<real,1> v = t_params.log() - t_grads * (eta * scale * gscale / model->weight_decay.current_weight_decay());
  Eigen::Tensor<real,1> v = t_params.log() + t_grads / model->weight_decay.current_weight_decay();// with momentum
  
  Eigen::Tensor<float,0> m = v.maximum();// max
  Eigen::Tensor<float,0> z = m.coeff() + (v - m.coeff()).exp().sum().log();// Z
  Eigen::Tensor<float,1> u = (v - z.coeff()).exp();// result
  TensorTools::SetElements(*ts[0], u.data(), u.dimensions()[0]);// for both GPU and CPU
}
DYNET_TRAINER_INST_DEV_IMPL(EGTrainer)

#ifndef __CUDACC__
void EGTrainer::update_params(real scale, real gscale, size_t idx) {
  auto & p = model->parameters_list()[idx];
  update_rule(scale, gscale, {&p->values, &p->g, &hp[idx].h});
}
void EGTrainer::update_lookup_params(real scale, real gscale, size_t idx, size_t lidx) {
  auto & p = model->lookup_parameters_list()[idx];
  update_rule(scale, gscale, {&p->values[lidx], &p->grads[lidx], &hlp[idx].h[lidx]});
}
void EGTrainer::update_lookup_params(real scale, real gscale, size_t idx) {
  auto & p = model->lookup_parameters_list()[idx];
  update_rule(scale, gscale, {&p->all_grads, &p->all_grads, &hlp[idx].all_h});
}
void EGTrainer::alloc_impl() {
  hp = allocate_shadow_parameters(*model);
  hlp = allocate_shadow_lookup_parameters(*model);
}
#endif

#ifndef __CUDACC__
// BOOST_CLASS_EXPORT_IMPLEMENT(dynet::SimpleSGDTrainer)
// BOOST_CLASS_EXPORT_IMPLEMENT(dynet::MomentumSGDTrainer)
// BOOST_CLASS_EXPORT_IMPLEMENT(dynet::AdagradTrainer)
// BOOST_CLASS_EXPORT_IMPLEMENT(dynet::AdadeltaTrainer)
// BOOST_CLASS_EXPORT_IMPLEMENT(dynet::RmsPropTrainer)
// BOOST_CLASS_EXPORT_IMPLEMENT(dynet::AdamTrainer)

template<class Archive>
void Trainer::serialize(Archive& ar, const unsigned int) {
  ar & eta0 & eta & eta_decay & epoch;
  ar & clipping_enabled & clip_threshold & clips & updates;
  ar & aux_allocated;
  ar & model;
}
DYNET_SERIALIZE_IMPL(Trainer)

template<class Archive>
void SimpleSGDTrainer::serialize(Archive& ar, const unsigned int) {
  ar & boost::serialization::base_object<Trainer>(*this);
}
DYNET_SERIALIZE_IMPL(SimpleSGDTrainer)

template<class Archive>
void MomentumSGDTrainer::serialize(Archive& ar, const unsigned int) {
  ar & boost::serialization::base_object<Trainer>(*this);
  ar & momentum;
  ar & vp & vlp;
}
DYNET_SERIALIZE_IMPL(MomentumSGDTrainer)

template<class Archive>
void AdagradTrainer::serialize(Archive& ar, const unsigned int) {
  ar & boost::serialization::base_object<Trainer>(*this);
  ar & epsilon;
  ar & vp & vlp;
}
DYNET_SERIALIZE_IMPL(AdagradTrainer)

template<class Archive>
void AdadeltaTrainer::serialize(Archive& ar, const unsigned int) {
  ar & boost::serialization::base_object<Trainer>(*this);
  ar & epsilon & rho;
  ar & hg & hlg & hd & hld;
}
DYNET_SERIALIZE_IMPL(AdadeltaTrainer)

template<class Archive>
void RmsPropTrainer::serialize(Archive& ar, const unsigned int) {
  ar & boost::serialization::base_object<Trainer>(*this);
  ar & epsilon & rho;
  ar & hg & hlg;
}
DYNET_SERIALIZE_IMPL(RmsPropTrainer)

template<class Archive>
void AdamTrainer::serialize(Archive& ar, const unsigned int) {
  ar & boost::serialization::base_object<Trainer>(*this);
  ar & beta_1 & beta_2 & epsilon;
  ar & m & lm & v & lv;
}
DYNET_SERIALIZE_IMPL(AdamTrainer)

template<class Archive>
void EGTrainer::serialize(Archive& ar, const unsigned int i) {
  ar & boost::serialization::base_object<Trainer>(*this);
  ar & momentum;
  ar & hp & hlp;
}
DYNET_SERIALIZE_IMPL(EGTrainer)

template<class Archive>
void AdaptiveEGTrainer::serialize(Archive& ar, const unsigned int i) {
  ar & boost::serialization::base_object<Trainer>(*this);
  ar & m & lm & v & lv;
}
DYNET_SERIALIZE_IMPL(AdaptiveEGTrainer)

#endif

} // namespace dynet

#ifndef __CUDACC__
BOOST_CLASS_EXPORT_IMPLEMENT(dynet::SimpleSGDTrainer)
BOOST_CLASS_EXPORT_IMPLEMENT(dynet::MomentumSGDTrainer)
BOOST_CLASS_EXPORT_IMPLEMENT(dynet::AdagradTrainer)
BOOST_CLASS_EXPORT_IMPLEMENT(dynet::AdadeltaTrainer)
BOOST_CLASS_EXPORT_IMPLEMENT(dynet::RmsPropTrainer)
BOOST_CLASS_EXPORT_IMPLEMENT(dynet::AdamTrainer)
BOOST_CLASS_EXPORT_IMPLEMENT(dynet::EGTrainer)
BOOST_CLASS_EXPORT_IMPLEMENT(dynet::AdaptiveEGTrainer)
#endif
