#include "splitting.h"

struct Step {
	static constexpr REP rep = REP::NONE;
	static constexpr bool late = false;
};
struct Time {
	static constexpr REP rep = REP::NONE;
	static constexpr bool late = false;
};

using ENERGY_TOTAL = SUM<AVG<PotentialEnergy>, AVG<KineticEnergy>>;
using ENERGY_DIFFERENCE = CHANGE<SUM<AVG<PotentialEnergy>, AVG<KineticEnergy>>>;

struct PropagatorBase
{
	double dt;
	ind max_steps;
	double state_accuracy;

	ind step{ 0 };
	double timer{ 0.0 };
	double timer_copy{ 0.0 };

	inline void incrementBy(double fraction = 1.0)
	{
		timer += dt * fraction;
	}
	inline void time_backup()
	{
		timer_copy = timer;
	}
	void time_restore()
	{
		timer = timer_copy;
	}
	void time_reset()
	{
		timer = 0;
	}
	void reset()
	{
		step = 0; timer = 0;
	}
};


template <MODE M, class SpType, class HamWF>
struct SplitPropagator : Config, PropagatorBase
{
	using SplitType = SpType;
	using ChainExpander = typename SplitType::ChainExpander;

	template <uind chain>
	using Chain = typename SplitType::template Chain<chain>;
	// template <uind chain>
	// using reps = typename Chain<chain>::reps;

	template <uind chain>
	using splits = typename Chain<chain>::splits;


	static constexpr uind ChainCount = ChainExpander::size;
	static constexpr REP firstREP = SplitType::firstREP;
	static constexpr REP invREP = REP::BOTH ^ firstREP;
	static constexpr std::string_view name = M == MODE::IM ? "IM" : "RE";
	static constexpr MODE mode = M;

	// static constexpr std::string_view name = SplitType::name;
	// static constexpr REP couplesInRep = C::couplesInRep;

	Section settings;
	HamWF wf;
	double kin_energy;
	double pot_energy;
	double tot_energy;
	double dif_energy;
#pragma region Initialization

	void autosetTimestep()
	{
		if (dt == 0.0)
		{
			// dt = 0.5 * pow(0.5, DIM - 1) * dx * dx;
			logWarning("Timestep dt was 0, using automatic value dt=%g", dt);
		}
	}

	SplitPropagator(PropagatorBase pb, HamWF wf) : PropagatorBase(pb), wf(wf)
	{
		logInfo("SplitPropagator init");
		max_steps = wf.coupling.maxPulseDuration() / dt + 1;
		logSETUP("maxPulseDuration: %g, dt: %g => max_steps: %td", wf.coupling.maxPulseDuration(), dt, max_steps);
		state_accuracy = 0;
	}

	SplitPropagator() :
		Config("project.ini", 1, 1),//DIMS, ELEC
		settings(ini.sections[name.data()]),
		wf(settings)
	{
		inipp::get_value(settings, "dt", dt);
		if constexpr (M == MODE::IM)
		{
			inipp::get_value(settings, "max_steps", max_steps);
			inipp::get_value(settings, "state_accuracy", state_accuracy);
			logInfo("max_steps %td", max_steps);
		}
		else
		{
			max_steps = wf.coupling.maxPulseDuration() / dt + 1;
			logSETUP("maxPulseDuration: %g, dt: %g => max_steps: %td", wf.coupling.maxPulseDuration(), dt, max_steps);
		}
		if constexpr (ChainCount > 1) wf.initHelpers();

		file_log = openLog(name);
		logInfo("dt %g", dt);
	}

	~SplitPropagator()
	{
		logInfo("SplitPropagation done!\n");
	}
#pragma endregion Initialization

#pragma region Computations
	template <class Op>
	inline double getValue()
	{
		if constexpr (std::is_same_v<Time, Op>) return timer;
		if constexpr (std::is_same_v<Step, Op>) return step;
		else return wf.template getValue<Op>();
	}

	//If no match here is found pass to the wavefunction with buffer
	template < REP R, class BO, class COMP>
	inline void compute(BO& bo, COMP&& c)
	{
		wf.template compute<M, R>(bo, std::forward<COMP>(c));
	}

	template < REP R, class BO, class ... Op>
	inline void compute(BO& bo, AVG<Op...>&& c)
	{
		using T = AVG<Op...>;
		bo.template store < T>((wf.template average<R, Op>())...);
	}
	template < REP R, class BO, class ... Op>
	inline void compute(BO& bo, OPERATION<Op...>&& c)
	{
		((wf.template operation<M, R, Op>()), ...);
	}
	template <REP R, class BO, class... Op>
	inline void compute(BO& bo, VALUE<Op...>&&)
	{
		using T = VALUE<Op...>;
		// bo.template store < T>(getValue(Op{}) ...);
		bo.template store < T>(getValue<Op>()...);
	}

	template <REP R, class BO, class... Op>
	inline void compute(BO& bo, SUM<Op...>&&)
	{
		using T = SUM<Op...>;
		bo.template store<T>((bo.template getLastValue<Op>() + ...));

		if constexpr (std::is_same_v<T, ENERGY_TOTAL>)
			tot_energy = bo.template getLastValue<T>();
	}

	template <REP R, class BO, class Op>
	inline void compute(BO& bo, CHANGE<Op>&&)
	{
		static double last = 0;
		using T = CHANGE<Op>;

		double curr = bo.template getLastValue<Op>();
		bo.template store<T>(curr - last);
		last = curr;

		if constexpr (std::is_same_v<T, ENERGY_DIFFERENCE>)
			dif_energy = bo.template getLastValue<T>();
	}

	inline void ditch() {}



	template <WHEN when, bool B, class... COMP>
	inline void computeEach(BufferedOutputs<B, COMP...>& bo)
	{
		// using BO = BufferedOutputs<B, COMP...>;

		//Run only if required REP is firstREP or no preference
		((!COMP::late && (COMP::rep == REP::NONE || bool(COMP::rep & firstREP))
		  ? compute<firstREP>(bo, COMP{})
		  : ditch()),
		 ...);

		//Check if work in inverse fourier space is needed
		if constexpr ((false || ... || (bool(COMP::rep & invREP))))
		{
			fourier<invREP>();
			((bool(COMP::rep & invREP)
			  ? compute<invREP>(bo, COMP{})
			  : ditch()),
			 ...);
			fourier<firstREP>();
		}
		else if (step < 1)
		{
			logWarning("Computations do not require FFT, if you need to output wavefunction in the opposite REP make sure to export the WF explicitly in the desired REP.");
		}

		//Check if any "late" operations are required
		if constexpr ((false || ... || (COMP::late)))
		{
			((COMP::late
			  ? compute<invREP>(bo, COMP{})
			  : ditch()),
			 ...);
		}

		bo.template logOrPass<M, when>(step);
	}
#pragma endregion Computations

#pragma region Evolution
	inline bool stillEvolving()
	{
		if constexpr (mode == MODE::RE) return (step <= max_steps);
		else return fabs(dif_energy) > state_accuracy && step != max_steps;
	}

	template <REP R>
	void fourier()
	{
		// logInfo("calling FFTW from propagator %d ", int(invREP));
		wf.template fourier<R>();
	}

	template <uind chain, uind ... SI>
	inline void chainEvolve(seq<SI...>)
	{
		([&] {
			constexpr REP rep = Chain<chain>::template rep<SI>;
			if (SI > 0) fourier<rep>();

			wf.template precalc<rep, OPTIMS::NONE>(timer);
			   //  Timings::measure::start(op.name);
			wf.template evolve<M, rep>(dt * Chain<chain>::mults[SI]);
		   //  Timings::measure::stop(op.name);
			if (step == 4)
				logInfo("SplitGroup %td Evolving in REP %td with delta=%g", chain, ind(rep), Chain<chain>::mults[SI]);
			if constexpr (REP::BOTH == HamWF::couplesInRep)
				incrementBy(Chain<chain>::mults[SI] * 0.5);
			else if constexpr (rep == HamWF::couplesInRep)
				incrementBy(Chain<chain>::mults[SI] * 1.0);
		 }(), ...);
	}

	template <uind... chain>
	inline void makeStep(seq<chain...>)
	{
		chainBackup();
		((
			chainRestore<chain>()
		//   , Timings::measure::start("EVOLUTION")
			, chainEvolve<chain>(splits<chain>{})
		  //   , Timings::measure::stop("EVOLUTION")
			, chainComplete<chain>()
			), ...);
		step++;
	}
#pragma endregion Evolution

#pragma region SplitChains

	template <ind chain> void chainRestore()
	{
		if constexpr (chain > 1)
		{
			if (step == 4) { logInfo("group %td restore", chain); }
			time_restore();
			wf.restore();
			// HamWF::Coupling::restore();
		}
	}
	template <ind chain> void chainComplete()
	{
		if constexpr (ChainCount > 1)
		{
			if (step == 4)
			{
				logInfo("group %td complete, applying coeff=%g", chain, Chain<chain>::value);
			}
			if constexpr (chain == ChainCount - 1)
				wf.collect(Chain<chain>::value);
			else wf.accumulate(Chain<chain>::value);

		}
	}
	void chainBackup()
	{
		if constexpr (ChainCount > 1)
		{
			if (step == 4) { logInfo("group backup because ChainCount=%td", ChainCount); }
			wf.backup();
			time_backup();
			// HamWF::Coupling::backup();
		}
	}
#pragma endregion splitChains

#pragma region Runner

	template <class OUTS, class Worker>
	void run(OUTS&& outputs, Worker&& worker, uind pass = 0)
	{
		outputs.template init<M>(pass, name);
		worker(WHEN::AT_START, step, pass, wf);
		computeEach<WHEN::AT_START>(outputs);
		while (stillEvolving())
		{
			makeStep(ChainExpander{});
			computeEach<WHEN::DURING>(outputs);
			wf.post_step();
			worker(WHEN::DURING, step, pass, wf);
		}

		makeStep(ChainExpander{});
		computeEach<WHEN::AT_END>(outputs);
		wf.post_step();
		worker(WHEN::AT_END, step, pass, wf);
	   // HACK: This makes sure, the steps are always evenly spaced
	   // step += (outputs.comp_interval - 1);
	   // Evolution::incrementBy(outputs.comp_interval);
	   /* if (imaginaryTimeQ)
	   {
		   Eigen::store<startREP>(state, PASS, energy);
		   Eigen::saveEnergyInfo(RT::name, state, PASS, energy, dE);
		   energy = 0;
		   energy_prev = 0;
		   dE = 0;
	   }  */
	}

	template <class OUTS>
	void run(OUTS&& outputs, uind pass = 0)
	{
		run<OUTS>(std::forward<OUTS>(outputs), [](WHEN when, ind step, uind pass, const auto& wf) {}, pass);
	}

	template <class OUTS, class Worker>
	void run(Worker&& worker, uind pass = 0)
	{
		run(std::move(OUTS{ settings }), std::forward<Worker>(worker), pass);
	}

	template <class OUTS>
	void run(uind pass = 0)
	{
		run<OUTS>([](WHEN when, ind step, uind pass, const auto& wf) {}, pass);
	}

#pragma endregion Runner

};

struct ADV_CONFIG
{
	bool discard_eigenstate_phase = true;
	double fs_postpropagation = 0.0;
	DATA_FORMAT data_format;
	DUMP_FORMAT dump_format;
};

/* Imagi time
struct ImagTime
{
	static constexpr std::string_view name = "IM";
	static constexpr MODE mode = IM;
	double operator()(double val)
	{
		return exp(-val);
	}
	inline bool exitCondition()
	{
		return !((fabs(dE) < state_accuracy || dE / energy == 0.) || step == max_imaginary_steps);
	}
	void config(Section& settings)
	{
		inipp::get_value(settings, "max_imaginary_steps", max_imaginary_steps);
		inipp::get_value(settings, "state_accuracy", state_accuracy);
	}
	inline void after()
	{
		// basePass::after();
		// Eigen::store<startREP>(state, PASS, energy);
		// Eigen::saveEnergyInfo(name, state, PASS, energy, dE);
		// energy = 0;
		// energy_prev = 0;
		// dE = 0;
	}
};
struct RealTime
{
	static constexpr std::string_view name = "RE";
	static constexpr MODE mode = RE;
	cxd operator()(double val)
	{
		return cos(-val) + I * sin(-val);
	}
	inline bool exitCondition()
	{
		return (step < ntsteps);
	}
};
*/

/* old get chain
SplitType splitSum;
	template <uind Chain>
	constexpr size_t splitCount() const { return splitSum.sizes[Chain]; };

	constexpr SplitPropagator() {}
	template <typename ... deltaMult>
	constexpr SplitPropagator(SplitType splitSum, double CAPlength, deltaMult...mult) :
		splitSum(splitSum) {}
			splits{ SplitOperators<T_Op, V_Op, C_Op>{mult, CAPlength} ... } {}


	template <size_t chain> constexpr auto getChain() const { return get<chain>(splitSum.chains); }

	template <size_t chain, size_t I> constexpr auto getOperator() const
	{
		return  get<I>(getChain<chain>().splits);
	}

	*/