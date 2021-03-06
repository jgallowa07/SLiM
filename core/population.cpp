//
//  population.cpp
//  SLiM
//
//  Created by Ben Haller on 12/13/14.
//  Copyright (c) 2014-2017 Philipp Messer.  All rights reserved.
//	A product of the Messer Lab, http://messerlab.org/slim/
//

//	This file is part of SLiM.
//
//	SLiM is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by
//	the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
//
//	SLiM is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
//	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more details.
//
//	You should have received a copy of the GNU General Public License along with SLiM.  If not, see <http://www.gnu.org/licenses/>.


#include "population.h"

#include <fstream>
#include <iomanip>
#include <algorithm>
#include <cmath>
#include <utility>
#include <unordered_map>

#include "slim_sim.h"
#include "slim_global.h"
#include "eidos_script.h"
#include "eidos_interpreter.h"
#include "eidos_symbol_table.h"
#include "polymorphism.h"


Population::Population(SLiMSim &p_sim) : sim_(p_sim)
{
}

Population::~Population(void)
{
	RemoveAllSubpopulationInfo();
	
#ifdef SLIMGUI
	// release malloced storage for SLiMgui statistics collection
	for (auto history_record_iter : fitness_histories_)
	{
		FitnessHistory &history_record = history_record_iter.second;
		
		if (history_record.history_)
		{
			free(history_record.history_);
			history_record.history_ = nullptr;
			history_record.history_length_ = 0;
		}
	}
#endif
	
	// dispose of any freed subpops
	for (auto removed_subpop : removed_subpops_)
		delete removed_subpop;
	
	removed_subpops_.clear();
}

void Population::RemoveAllSubpopulationInfo(void)
{
	// Free all subpopulations and then clear out our subpopulation list
	for (auto subpopulation : *this)
		delete subpopulation.second;
	
	this->clear();
	
	// Free all substitutions and clear out the substitution vector
	for (auto substitution : substitutions_)
		delete substitution;
	
	substitutions_.clear();
	
	// The malloced storage of mutation_registry_ will be freed when it is destroyed, but it
	// does not know that the Mutation pointers inside it are owned, so we need to free them.
	const MutationIndex *registry_iter = mutation_registry_.begin_pointer_const();
	const MutationIndex *registry_iter_end = mutation_registry_.end_pointer_const();
	
	for (; registry_iter != registry_iter_end; ++registry_iter)
	{
		MutationIndex mutation = *registry_iter;
		
		// We no longer delete mutation objects; instead, we remove them from our shared pool
		(gSLiM_Mutation_Block + mutation)->~Mutation();
		SLiM_DisposeMutationToBlock(mutation);
	}
	
	mutation_registry_.clear();
	
#ifdef SLIMGUI
	// release malloced storage for SLiMgui statistics collection
	if (mutation_loss_times_)
	{
		free(mutation_loss_times_);
		mutation_loss_times_ = nullptr;
		mutation_loss_gen_slots_ = 0;
	}
	if (mutation_fixation_times_)
	{
		free(mutation_fixation_times_);
		mutation_fixation_times_ = nullptr;
		mutation_fixation_gen_slots_ = 0;
	}
	// Don't throw away the fitness history; it is perfectly valid even if the population has just been changed completely.  It happened.
	// If the read is followed by setting the generation backward, individual fitness history entries will be invalidated in response.
//	if (fitness_history_)
//	{
//		free(fitness_history_);
//		fitness_history_ = nullptr;
//		fitness_history_length_ = 0;
//	}
#endif
}

// add new empty subpopulation p_subpop_id of size p_subpop_size
Subpopulation *Population::AddSubpopulation(slim_objectid_t p_subpop_id, slim_popsize_t p_subpop_size, double p_initial_sex_ratio) 
{ 
	if (count(p_subpop_id) != 0)
		EIDOS_TERMINATION << "ERROR (Population::AddSubpopulation): subpopulation p" << p_subpop_id << " already exists." << EidosTerminate();
	if (p_subpop_size < 1)
		EIDOS_TERMINATION << "ERROR (Population::AddSubpopulation): subpopulation p" << p_subpop_id << " empty." << EidosTerminate();
	
	// make and add the new subpopulation
	Subpopulation *new_subpop = nullptr;
	
	if (sim_.SexEnabled())
		new_subpop = new Subpopulation(*this, p_subpop_id, p_subpop_size, p_initial_sex_ratio, sim_.ModeledChromosomeType(), sim_.XDominanceCoefficient());	// SEX ONLY
	else
		new_subpop = new Subpopulation(*this, p_subpop_id, p_subpop_size);
	
	new_subpop->child_generation_valid_ = child_generation_valid_;	// synchronize its stage with ours
	
#ifdef SLIMGUI
	// When running under SLiMgui, we need to decide whether this subpopulation comes in selected or not.  We can't defer that
	// to SLiMgui's next update, because then mutation tallies are not kept properly up to date, resulting in a bad GUI refresh.
	// The rule is: if all currently existing subpops are selected, then the new subpop comes in selected as well.
	new_subpop->gui_selected_ = gui_all_selected_;
#endif
	
	insert(std::pair<const slim_objectid_t,Subpopulation*>(p_subpop_id, new_subpop));
	
	return new_subpop;
}

// add new subpopulation p_subpop_id of size p_subpop_size individuals drawn from source subpopulation p_source_subpop_id
Subpopulation *Population::AddSubpopulation(slim_objectid_t p_subpop_id, Subpopulation &p_source_subpop, slim_popsize_t p_subpop_size, double p_initial_sex_ratio)
{
	if (count(p_subpop_id) != 0)
		EIDOS_TERMINATION << "ERROR (Population::AddSubpopulation): subpopulation p" << p_subpop_id << " already exists." << EidosTerminate();
	if (p_subpop_size < 1)
		EIDOS_TERMINATION << "ERROR (Population::AddSubpopulation): subpopulation p" << p_subpop_id << " empty." << EidosTerminate();
	
	// make and add the new subpopulation
	Subpopulation *new_subpop = nullptr;
 
	if (sim_.SexEnabled())
		new_subpop = new Subpopulation(*this, p_subpop_id, p_subpop_size, p_initial_sex_ratio, sim_.ModeledChromosomeType(), sim_.XDominanceCoefficient());	// SEX ONLY
	else
		new_subpop = new Subpopulation(*this, p_subpop_id, p_subpop_size);
	
	new_subpop->child_generation_valid_ = child_generation_valid_;	// synchronize its stage with ours
	
#ifdef SLIMGUI
	// When running under SLiMgui, we need to decide whether this subpopulation comes in selected or not.  We can't defer that
	// to SLiMgui's next update, because then mutation tallies are not kept properly up to date, resulting in a bad GUI refresh.
	// The rule is: if all currently existing subpops are selected, then the new subpop comes in selected as well.
	new_subpop->gui_selected_ = gui_all_selected_;
#endif
	
	insert(std::pair<const slim_objectid_t,Subpopulation*>(p_subpop_id, new_subpop));
	
	// then draw parents from the source population according to fitness, obeying the new subpop's sex ratio
	Subpopulation &subpop = *new_subpop;
	
	for (slim_popsize_t parent_index = 0; parent_index < subpop.parent_subpop_size_; parent_index++)
	{
		// draw individual from p_source_subpop and assign to be a parent in subpop
		slim_popsize_t migrant_index;
		
		if (sim_.SexEnabled())
		{
			if (parent_index < subpop.parent_first_male_index_)
				migrant_index = p_source_subpop.DrawFemaleParentUsingFitness();
			else
				migrant_index = p_source_subpop.DrawMaleParentUsingFitness();
		}
		else
		{
			migrant_index = p_source_subpop.DrawParentUsingFitness();
		}
		
		subpop.parent_genomes_[2 * parent_index].copy_from_genome(p_source_subpop.parent_genomes_[2 * migrant_index]);
		subpop.parent_genomes_[2 * parent_index + 1].copy_from_genome(p_source_subpop.parent_genomes_[2 * migrant_index + 1]);
	}
	
	// UpdateFitness() is not called here - all fitnesses are kept as equal.  This is because the parents were drawn from the source subpopulation according
	// to their fitness already; fitness has already been applied.  If UpdateFitness() were called, fitness would be double-applied in this generation.
	
	return new_subpop;
}

// set size of subpopulation p_subpop_id to p_subpop_size
void Population::SetSize(Subpopulation &p_subpop, slim_popsize_t p_subpop_size)
{
	// SetSize() can only be called when the child generation has not yet been generated.  It sets the size on the child generation,
	// and then that size takes effect when the children are generated from the parents in EvolveSubpopulation().
	if (child_generation_valid_)
		EIDOS_TERMINATION << "ERROR (Population::SetSize): called when the child generation was valid." << EidosTerminate();
	
	if (p_subpop_size == 0) // remove subpopulation p_subpop_id
	{
		// Note that we don't free the subpopulation here, because there may be live references to it; instead we keep it to the end of the generation and then free it
		// First we remove the symbol for the subpop
		sim_.SymbolTable().RemoveConstantForSymbol(p_subpop.SymbolTableEntry().first);
		
		// Then we immediately remove the subpop from our list of subpops
		slim_objectid_t subpop_id = p_subpop.subpopulation_id_;
		
		erase(subpop_id);
		
		for (std::pair<const slim_objectid_t,Subpopulation*> &subpop_pair : *this)
			subpop_pair.second->migrant_fractions_.erase(subpop_id);
		
		// remember the subpop for later disposal
		removed_subpops_.emplace_back(&p_subpop);
	}
	else
	{
		// After we change the subpop size, we need to generate new children genomes to fit the new requirements
		p_subpop.child_subpop_size_ = p_subpop_size;
		p_subpop.GenerateChildrenToFit(false);	// false means generate only new children, not new parents
	}
}

// set fraction p_migrant_fraction of p_subpop_id that originates as migrants from p_source_subpop_id per generation  
void Population::SetMigration(Subpopulation &p_subpop, slim_objectid_t p_source_subpop_id, double p_migrant_fraction) 
{ 
	if (count(p_source_subpop_id) == 0)
		EIDOS_TERMINATION << "ERROR (Population::SetMigration): no subpopulation p" << p_source_subpop_id << "." << EidosTerminate();
	if (p_migrant_fraction < 0.0 || p_migrant_fraction > 1.0)
		EIDOS_TERMINATION << "ERROR (Population::SetMigration): migration fraction has to be within [0,1] (" << p_migrant_fraction << " supplied)." << EidosTerminate();
	
	if (p_subpop.migrant_fractions_.count(p_source_subpop_id) != 0)
		p_subpop.migrant_fractions_.erase(p_source_subpop_id);
	
	if (p_migrant_fraction > 0.0)	// BCH 4 March 2015: Added this if so we don't put a 0.0 migration rate into the table; harmless but looks bad in SLiMgui...
		p_subpop.migrant_fractions_.insert(std::pair<const slim_objectid_t,double>(p_source_subpop_id, p_migrant_fraction)); 
}

// execute a script event in the population; the script is assumed to be due to trigger
void Population::ExecuteScript(SLiMEidosBlock *p_script_block, slim_generation_t p_generation, const Chromosome &p_chromosome)
{
#pragma unused(p_generation, p_chromosome)
	EidosSymbolTable callback_symbols(EidosSymbolTableType::kContextConstantsTable, &sim_.SymbolTable());
	EidosSymbolTable client_symbols(EidosSymbolTableType::kVariablesTable, &callback_symbols);
	EidosFunctionMap &function_map = sim_.FunctionMap();
	
	EidosInterpreter interpreter(p_script_block->compound_statement_node_, client_symbols, function_map, &sim_);
	
	if (p_script_block->contains_self_)
		callback_symbols.InitializeConstantSymbolEntry(p_script_block->SelfSymbolTableEntry());		// define "self"
	
	try
	{
		// Interpret the script; the result from the interpretation is not used for anything
		EidosValue_SP result = interpreter.EvaluateInternalBlock(p_script_block->script_);
		
		// Output generated by the interpreter goes to our output stream
		SLIM_OUTSTREAM << interpreter.ExecutionOutput();
	}
	catch (...)
	{
		// Emit final output even on a throw, so that stop() messages and such get printed
		SLIM_OUTSTREAM << interpreter.ExecutionOutput();
		
		throw;
	}
}

// apply mateChoice() callbacks to a mating event with a chosen first parent; the return is the second parent index, or -1 to force a redraw
slim_popsize_t Population::ApplyMateChoiceCallbacks(slim_popsize_t p_parent1_index, Subpopulation *p_subpop, Subpopulation *p_source_subpop, std::vector<SLiMEidosBlock*> &p_mate_choice_callbacks)
{
#if defined(SLIMGUI) && (SLIMPROFILING == 1)
	// PROFILING
	SLIM_PROFILE_BLOCK_START();
#endif
	
	// We start out using standard weights taken from the source subpopulation.  If, when we are done handling callbacks, we are still
	// using those standard weights, then we can do a draw using our fast lookup tables.  Otherwise, we will do a draw the hard way.
	bool sex_enabled = p_subpop->sex_enabled_;
	double *standard_weights = (sex_enabled ? p_source_subpop->cached_male_fitness_ : p_source_subpop->cached_parental_fitness_);
	double *current_weights = standard_weights;
	slim_popsize_t weights_length = p_source_subpop->cached_fitness_size_;
	bool weights_modified = false;
	Individual *chosen_mate = nullptr;			// callbacks can return an Individual instead of a weights vector, held here
	bool weights_reflect_chosen_mate = false;	// if T, a weights vector has been created with a 1 for the chosen mate, to pass to the next callback
	SLiMEidosBlock *last_interventionist_mate_choice_callback = nullptr;
	
	for (SLiMEidosBlock *mate_choice_callback : p_mate_choice_callbacks)
	{
		if (mate_choice_callback->active_)
		{
			// local variables for the callback parameters that we might need to allocate here, and thus need to free below
			EidosValue_SP local_weights_ptr;
			bool redraw_mating = false;
			
			if (chosen_mate && !weights_reflect_chosen_mate && mate_choice_callback->contains_weights_)
			{
				// A previous callback said it wanted a specific individual to be the mate.  We now need to make a weights vector
				// to represent that, since we have another callback that wants an incoming weights vector.
				if (!weights_modified)
				{
					current_weights = (double *)malloc(sizeof(double) * weights_length);	// allocate a new weights vector
					weights_modified = true;
				}
				
				EIDOS_BZERO(current_weights, sizeof(double) * weights_length);
				current_weights[chosen_mate->IndexInSubpopulation()] = 1.0;
				
				weights_reflect_chosen_mate = true;
			}
			
			// The callback is active, so we need to execute it; we start a block here to manage the lifetime of the symbol table
			{
				EidosSymbolTable callback_symbols(EidosSymbolTableType::kContextConstantsTable, &sim_.SymbolTable());
				EidosSymbolTable client_symbols(EidosSymbolTableType::kVariablesTable, &callback_symbols);
				EidosFunctionMap &function_map = sim_.FunctionMap();
				EidosInterpreter interpreter(mate_choice_callback->compound_statement_node_, client_symbols, function_map, &sim_);
				
				if (mate_choice_callback->contains_self_)
					callback_symbols.InitializeConstantSymbolEntry(mate_choice_callback->SelfSymbolTableEntry());		// define "self"
				
				// Set all of the callback's parameters; note we use InitializeConstantSymbolEntry() for speed.
				// We can use that method because we know the lifetime of the symbol table is shorter than that of
				// the value objects, and we know that the values we are setting here will not change (the objects
				// referred to by the values may change, but the values themselves will not change).
				if (mate_choice_callback->contains_individual_)
				{
					Individual *parent1 = &(p_source_subpop->parent_individuals_[p_parent1_index]);
					callback_symbols.InitializeConstantSymbolEntry(gID_individual, parent1->CachedEidosValue());
				}
				
				if (mate_choice_callback->contains_genome1_)
				{
					Genome *parent1_genome1 = &(p_source_subpop->parent_genomes_[p_parent1_index * 2]);
					callback_symbols.InitializeConstantSymbolEntry(gID_genome1, parent1_genome1->CachedEidosValue());
				}
				
				if (mate_choice_callback->contains_genome2_)
				{
					Genome *parent1_genome2 = &(p_source_subpop->parent_genomes_[p_parent1_index * 2 + 1]);
					callback_symbols.InitializeConstantSymbolEntry(gID_genome2, parent1_genome2->CachedEidosValue());
				}
				
				if (mate_choice_callback->contains_subpop_)
					callback_symbols.InitializeConstantSymbolEntry(gID_subpop, p_subpop->SymbolTableEntry().second);
				
				if (mate_choice_callback->contains_sourceSubpop_)
					callback_symbols.InitializeConstantSymbolEntry(gID_sourceSubpop, p_source_subpop->SymbolTableEntry().second);
				
				if (mate_choice_callback->contains_weights_)
				{
					local_weights_ptr = EidosValue_SP(new (gEidosValuePool->AllocateChunk()) EidosValue_Float_vector(current_weights, weights_length));
					callback_symbols.InitializeConstantSymbolEntry(gEidosID_weights, local_weights_ptr);
				}
				
				try
				{
					// Interpret the script; the result from the interpretation can be one of several things, so this is a bit complicated
					EidosValue_SP result_SP = interpreter.EvaluateInternalBlock(mate_choice_callback->script_);
					EidosValue *result = result_SP.get();
					
					if (result->Type() == EidosValueType::kValueNULL)
					{
						// NULL indicates that the mateChoice() callback did not wish to alter the weights, so we do nothing
					}
					else if (result->Type() == EidosValueType::kValueObject)
					{
						// A singleton vector of type Individual may be returned to choose a specific mate
						if ((result->Count() == 1) && (((EidosValue_Object *)result)->Class() == gSLiM_Individual_Class))
						{
							chosen_mate = (Individual *)result->ObjectElementAtIndex(0, mate_choice_callback->identifier_token_);
							weights_reflect_chosen_mate = false;
							
							// remember this callback for error attribution below
							last_interventionist_mate_choice_callback = mate_choice_callback;
						}
						else
						{
							EIDOS_TERMINATION << "ERROR (Population::ApplyMateChoiceCallbacks): invalid return value for mateChoice() callback." << EidosTerminate(mate_choice_callback->identifier_token_);
						}
					}
					else if (result->Type() == EidosValueType::kValueFloat)
					{
						int result_count = result->Count();
						
						if (result_count == 0)
						{
							// a return of float(0) indicates that there is no acceptable mate for the first parent; the first parent must be redrawn
							redraw_mating = true;
						}
						else if (result_count == weights_length)
						{
							// if we used to have a specific chosen mate, we don't any more
							chosen_mate = nullptr;
							weights_reflect_chosen_mate = false;
							
							// a non-zero float vector must match the size of the source subpop, and provides a new set of weights for us to use
							if (!weights_modified)
							{
								current_weights = (double *)malloc(sizeof(double) * weights_length);	// allocate a new weights vector
								weights_modified = true;
							}
							
							// We really want to use EidosValue_Float_vector's FloatVector() method to get the values; if the dynamic_cast
							// fails, we presumably have an EidosValue_Float_singleton and must get its value with FloatAtIndex.
							EidosValue_Float_vector *result_vector_type = dynamic_cast<EidosValue_Float_vector *>(result);
							
							if (result_vector_type)
								memcpy(current_weights, result_vector_type->FloatVector()->data(), sizeof(double) * weights_length);
							else
								current_weights[0] = result->FloatAtIndex(0, nullptr);
							
							// remember this callback for error attribution below
							last_interventionist_mate_choice_callback = mate_choice_callback;
						}
						else
						{
							EIDOS_TERMINATION << "ERROR (Population::ApplyMateChoiceCallbacks): invalid return value for mateChoice() callback." << EidosTerminate(mate_choice_callback->identifier_token_);
						}
					}
					else
					{
						EIDOS_TERMINATION << "ERROR (Population::ApplyMateChoiceCallbacks): invalid return value for mateChoice() callback." << EidosTerminate(mate_choice_callback->identifier_token_);
					}
					
					// Output generated by the interpreter goes to our output stream
					SLIM_OUTSTREAM << interpreter.ExecutionOutput();
				}
				catch (...)
				{
					// Emit final output even on a throw, so that stop() messages and such get printed
					SLIM_OUTSTREAM << interpreter.ExecutionOutput();
					
					throw;
				}
			}
			
			// If this callback told us not to generate the child, we do not call the rest of the callback chain; we're done
			if (redraw_mating)
			{
				if (weights_modified)
					free(current_weights);
				
#if defined(SLIMGUI) && (SLIMPROFILING == 1)
				// PROFILING
				SLIM_PROFILE_BLOCK_END(sim_.profile_callback_totals_[(int)(SLiMEidosBlockType::SLiMEidosMateChoiceCallback)]);
#endif
				
				return -1;
			}
		}
	}
	
	// If we have a specific chosen mate, then we don't need to draw, but we do need to check the sex of the proposed mate
	if (chosen_mate)
	{
		slim_popsize_t drawn_parent = chosen_mate->IndexInSubpopulation();
		
		if (weights_modified)
			free(current_weights);
		
		if (sex_enabled)
		{
			if (drawn_parent < p_source_subpop->parent_first_male_index_)
				EIDOS_TERMINATION << "ERROR (Population::ApplyMateChoiceCallbacks): second parent chosen by mateChoice() callback is female." << EidosTerminate(last_interventionist_mate_choice_callback->identifier_token_);
		}
		
#if defined(SLIMGUI) && (SLIMPROFILING == 1)
		// PROFILING
		SLIM_PROFILE_BLOCK_END(sim_.profile_callback_totals_[(int)(SLiMEidosBlockType::SLiMEidosMateChoiceCallback)]);
#endif
		
		return drawn_parent;
	}
	
	// If a callback supplied a different set of weights, we need to use those weights to draw a male parent
	if (weights_modified)
	{
		slim_popsize_t drawn_parent = -1;
		double weights_sum = 0;
		int positive_count = 0;
		
		// first we assess the weights vector: get its sum, bounds-check it, etc.
		for (slim_popsize_t weight_index = 0; weight_index < weights_length; ++weight_index)
		{
			double x = current_weights[weight_index];
			
			if (!std::isfinite(x))
				EIDOS_TERMINATION << "ERROR (Population::ApplyMateChoiceCallbacks): weight returned by mateChoice() callback is not finite." << EidosTerminate(last_interventionist_mate_choice_callback->identifier_token_);
			
			if (x > 0.0)
			{
				positive_count++;
				weights_sum += x;
				continue;
			}
			
			if (x < 0.0)
				EIDOS_TERMINATION << "ERROR (Population::ApplyMateChoiceCallbacks): weight returned by mateChoice() callback is less than 0.0." << EidosTerminate(last_interventionist_mate_choice_callback->identifier_token_);
		}
		
		if (weights_sum <= 0.0)
		{
			// We used to consider this an error; now we consider it to represent the first parent having no acceptable choice, so we
			// re-draw.  Returning float(0) is essentially equivalent, except that it short-circuits the whole mateChoice() callback
			// chain, whereas returning a vector of 0 values can be modified by a downstream mateChoice() callback.  Usually that is
			// not an important distinction.  Returning float(0) is faster in principle, but if one is already constructing a vector
			// of weights that can simply end up being all zero, then this path is much easier.  BCH 5 March 2017
			//EIDOS_TERMINATION << "ERROR (Population::ApplyMateChoiceCallbacks): weights returned by mateChoice() callback sum to 0.0 or less." << EidosTerminate(last_interventionist_mate_choice_callback->identifier_token_);
			if (weights_modified)
				free(current_weights);
			
#if defined(SLIMGUI) && (SLIMPROFILING == 1)
			// PROFILING
			SLIM_PROFILE_BLOCK_END(sim_.profile_callback_totals_[(int)(SLiMEidosBlockType::SLiMEidosMateChoiceCallback)]);
#endif
			
			return -1;
		}
		
		// then we draw from the weights vector
		if (positive_count == 1)
		{
			// there is only a single positive value, so the callback has chosen a parent for us; we just need to locate it
			// we could have noted it above, but I don't want to slow down that loop, since many positive weights is the likely case
			for (slim_popsize_t weight_index = 0; weight_index < weights_length; ++weight_index)
				if (current_weights[weight_index] > 0.0)
				{
					drawn_parent = weight_index;
					break;
				}
		}
		else if (positive_count <= weights_length / 4)	// the threshold here is a guess
		{
			// there are just a few positive values, so try to be faster about scanning for them by checking for zero first
			double the_rose_in_the_teeth = gsl_rng_uniform_pos(gEidos_rng) * weights_sum;
			double bachelor_sum = 0.0;
			
			for (slim_popsize_t weight_index = 0; weight_index < weights_length; ++weight_index)
			{
				double weight = current_weights[weight_index];
				
				if (weight > 0.0)
				{
					bachelor_sum += weight;
					
					if (the_rose_in_the_teeth <= bachelor_sum)
					{
						drawn_parent = weight_index;
						break;
					}
				}
			}
		}
		else
		{
			// there are many positive values, so we need to do a uniform draw and see who gets the rose
			double the_rose_in_the_teeth = gsl_rng_uniform_pos(gEidos_rng) * weights_sum;
			double bachelor_sum = 0.0;
			
			for (slim_popsize_t weight_index = 0; weight_index < weights_length; ++weight_index)
			{
				bachelor_sum += current_weights[weight_index];
				
				if (the_rose_in_the_teeth <= bachelor_sum)
				{
					drawn_parent = weight_index;
					break;
				}
			}
		}
		
		// we should always have a chosen parent at this point
		if (drawn_parent == -1)
			EIDOS_TERMINATION << "ERROR (Population::ApplyMateChoiceCallbacks): failed to choose a mate." << EidosTerminate(last_interventionist_mate_choice_callback->identifier_token_);
		
		free(current_weights);
		
		if (sex_enabled)
		{
			if (drawn_parent < p_source_subpop->parent_first_male_index_)
				EIDOS_TERMINATION << "ERROR (Population::ApplyMateChoiceCallbacks): second parent chosen by mateChoice() callback is female." << EidosTerminate(last_interventionist_mate_choice_callback->identifier_token_);
		}
		
#if defined(SLIMGUI) && (SLIMPROFILING == 1)
		// PROFILING
		SLIM_PROFILE_BLOCK_END(sim_.profile_callback_totals_[(int)(SLiMEidosBlockType::SLiMEidosMateChoiceCallback)]);
#endif
		
		return drawn_parent;
	}
	
#if defined(SLIMGUI) && (SLIMPROFILING == 1)
	// PROFILING
	SLIM_PROFILE_BLOCK_END(sim_.profile_callback_totals_[(int)(SLiMEidosBlockType::SLiMEidosMateChoiceCallback)]);
#endif
	
	// The standard behavior, with no active callbacks, is to draw a male parent using the standard fitness values
	return (sex_enabled ? p_source_subpop->DrawMaleParentUsingFitness() : p_source_subpop->DrawParentUsingFitness());
}

// apply modifyChild() callbacks to a generated child; a return of false means "do not use this child, generate a new one"
bool Population::ApplyModifyChildCallbacks(slim_popsize_t p_child_index, IndividualSex p_child_sex, slim_popsize_t p_parent1_index, slim_popsize_t p_parent2_index, bool p_is_selfing, bool p_is_cloning, Subpopulation *p_subpop, Subpopulation *p_source_subpop, std::vector<SLiMEidosBlock*> &p_modify_child_callbacks)
{
#if defined(SLIMGUI) && (SLIMPROFILING == 1)
	// PROFILING
	SLIM_PROFILE_BLOCK_START();
#endif
	
	for (SLiMEidosBlock *modify_child_callback : p_modify_child_callbacks)
	{
		if (modify_child_callback->active_)
		{
			// The callback is active, so we need to execute it
			EidosSymbolTable callback_symbols(EidosSymbolTableType::kContextConstantsTable, &sim_.SymbolTable());
			EidosSymbolTable client_symbols(EidosSymbolTableType::kVariablesTable, &callback_symbols);
			EidosFunctionMap &function_map = sim_.FunctionMap();
			EidosInterpreter interpreter(modify_child_callback->compound_statement_node_, client_symbols, function_map, &sim_);
			
			if (modify_child_callback->contains_self_)
				callback_symbols.InitializeConstantSymbolEntry(modify_child_callback->SelfSymbolTableEntry());		// define "self"
			
			// Set all of the callback's parameters; note we use InitializeConstantSymbolEntry() for speed.
			// We can use that method because we know the lifetime of the symbol table is shorter than that of
			// the value objects, and we know that the values we are setting here will not change (the objects
			// referred to by the values may change, but the values themselves will not change).
			if (modify_child_callback->contains_child_)
			{
				Individual *child = &(p_subpop->child_individuals_[p_child_index]);
				callback_symbols.InitializeConstantSymbolEntry(gID_child, child->CachedEidosValue());
			}
			
			if (modify_child_callback->contains_childGenome1_)
			{
				Genome *child_genome1 = &(p_subpop->child_genomes_[p_child_index * 2]);
				callback_symbols.InitializeConstantSymbolEntry(gID_childGenome1, child_genome1->CachedEidosValue());
			}
			
			if (modify_child_callback->contains_childGenome2_)
			{
				Genome *child_genome2 = &(p_subpop->child_genomes_[p_child_index * 2 + 1]);
				callback_symbols.InitializeConstantSymbolEntry(gID_childGenome2, child_genome2->CachedEidosValue());
			}
			
			if (modify_child_callback->contains_childIsFemale_)
			{
				if (p_child_sex == IndividualSex::kHermaphrodite)
					callback_symbols.InitializeConstantSymbolEntry(gID_childIsFemale, gStaticEidosValueNULL);
				else
					callback_symbols.InitializeConstantSymbolEntry(gID_childIsFemale, (p_child_sex == IndividualSex::kFemale) ? gStaticEidosValue_LogicalT : gStaticEidosValue_LogicalF);
			}
			
			if (modify_child_callback->contains_parent1_)
			{
				Individual *parent1 = &(p_source_subpop->parent_individuals_[p_parent1_index]);
				callback_symbols.InitializeConstantSymbolEntry(gID_parent1, parent1->CachedEidosValue());
			}
			
			if (modify_child_callback->contains_parent1Genome1_)
			{
				Genome *parent1_genome1 = &(p_source_subpop->parent_genomes_[p_parent1_index * 2]);
				callback_symbols.InitializeConstantSymbolEntry(gID_parent1Genome1, parent1_genome1->CachedEidosValue());
			}
			
			if (modify_child_callback->contains_parent1Genome2_)
			{
				Genome *parent1_genome2 = &(p_source_subpop->parent_genomes_[p_parent1_index * 2 + 1]);
				callback_symbols.InitializeConstantSymbolEntry(gID_parent1Genome2, parent1_genome2->CachedEidosValue());
			}
			
			if (modify_child_callback->contains_isSelfing_)
				callback_symbols.InitializeConstantSymbolEntry(gID_isSelfing, p_is_selfing ? gStaticEidosValue_LogicalT : gStaticEidosValue_LogicalF);
			
			if (modify_child_callback->contains_isCloning_)
				callback_symbols.InitializeConstantSymbolEntry(gID_isCloning, p_is_cloning ? gStaticEidosValue_LogicalT : gStaticEidosValue_LogicalF);
			
			if (modify_child_callback->contains_parent2_)
			{
				Individual *parent2 = &(p_source_subpop->parent_individuals_[p_parent2_index]);
				callback_symbols.InitializeConstantSymbolEntry(gID_parent2, parent2->CachedEidosValue());
			}
			
			if (modify_child_callback->contains_parent2Genome1_)
			{
				Genome *parent2_genome1 = &(p_source_subpop->parent_genomes_[p_parent2_index * 2]);
				callback_symbols.InitializeConstantSymbolEntry(gID_parent2Genome1, parent2_genome1->CachedEidosValue());
			}
			
			if (modify_child_callback->contains_parent2Genome2_)
			{
				Genome *parent2_genome2 = &(p_source_subpop->parent_genomes_[p_parent2_index * 2 + 1]);
				callback_symbols.InitializeConstantSymbolEntry(gID_parent2Genome2, parent2_genome2->CachedEidosValue());
			}
			
			if (modify_child_callback->contains_subpop_)
				callback_symbols.InitializeConstantSymbolEntry(gID_subpop, p_subpop->SymbolTableEntry().second);
			
			if (modify_child_callback->contains_sourceSubpop_)
				callback_symbols.InitializeConstantSymbolEntry(gID_sourceSubpop, p_source_subpop->SymbolTableEntry().second);
			
			try
			{
				// Interpret the script; the result from the interpretation must be a singleton double used as a new fitness value
				EidosValue_SP result_SP = interpreter.EvaluateInternalBlock(modify_child_callback->script_);
				EidosValue *result = result_SP.get();
				
				if ((result->Type() != EidosValueType::kValueLogical) || (result->Count() != 1))
					EIDOS_TERMINATION << "ERROR (Population::ApplyModifyChildCallbacks): modifyChild() callbacks must provide a logical singleton return value." << EidosTerminate(modify_child_callback->identifier_token_);
				
				eidos_logical_t generate_child = result->LogicalAtIndex(0, nullptr);
				
				// Output generated by the interpreter goes to our output stream
				SLIM_OUTSTREAM << interpreter.ExecutionOutput();
				
				// If this callback told us not to generate the child, we do not call the rest of the callback chain; we're done
				if (!generate_child)
				{
#if defined(SLIMGUI) && (SLIMPROFILING == 1)
					// PROFILING
					SLIM_PROFILE_BLOCK_END(sim_.profile_callback_totals_[(int)(SLiMEidosBlockType::SLiMEidosModifyChildCallback)]);
#endif
					
					return false;
				}
			}
			catch (...)
			{
				// Emit final output even on a throw, so that stop() messages and such get printed
				SLIM_OUTSTREAM << interpreter.ExecutionOutput();
				
				throw;
			}
		}
	}
	
#if defined(SLIMGUI) && (SLIMPROFILING == 1)
	// PROFILING
	SLIM_PROFILE_BLOCK_END(sim_.profile_callback_totals_[(int)(SLiMEidosBlockType::SLiMEidosModifyChildCallback)]);
#endif
	
	return true;
}

// generate children for subpopulation p_subpop_id, drawing from all source populations, handling crossover and mutation
void Population::EvolveSubpopulation(Subpopulation &p_subpop, const Chromosome &p_chromosome, slim_generation_t p_generation, bool p_mate_choice_callbacks_present, bool p_modify_child_callbacks_present, bool p_recombination_callbacks_present)
{
	bool pedigrees_enabled = sim_.PedigreesEnabled();
	bool prevent_incidental_selfing = sim_.PreventIncidentalSelfing();
	bool sex_enabled = p_subpop.sex_enabled_;
	slim_popsize_t total_children = p_subpop.child_subpop_size_;
	
	// set up to draw migrants; this works the same in the sex and asex cases, and for males / females / hermaphrodites
	// the way the code is now structured, "migrant" really includes everybody; we are a migrant source subpop for ourselves
	int migrant_source_count = static_cast<int>(p_subpop.migrant_fractions_.size());
	double migration_rates[migrant_source_count + 1];
	Subpopulation *migration_sources[migrant_source_count + 1];
	unsigned int num_migrants[migrant_source_count + 1];			// used by client code below; type constrained by gsl_ran_multinomial()
	
	if (migrant_source_count > 0)
	{
		double migration_rate_sum = 0.0;
		int pop_count = 0;
		
		for (const std::pair<const slim_objectid_t,double> &fractions_pair : p_subpop.migrant_fractions_)
		{
			slim_objectid_t migrant_source_id = fractions_pair.first;
			auto migrant_source_pair = find(migrant_source_id);
			
			if (migrant_source_pair == end())
				EIDOS_TERMINATION << "ERROR (Population::EvolveSubpopulation): no migrant source subpopulation p" << migrant_source_id << "." << EidosTerminate();
			
			migration_rates[pop_count] = fractions_pair.second;
			migration_sources[pop_count] = migrant_source_pair->second;
			migration_rate_sum += fractions_pair.second;
			pop_count++;
		}
		
		if (migration_rate_sum <= 1.0)
		{
			// the remaining fraction is within-subpopulation mating
			migration_rates[pop_count] = 1.0 - migration_rate_sum;
			migration_sources[pop_count] = &p_subpop;
		}
		else
			EIDOS_TERMINATION << "ERROR (Population::EvolveSubpopulation): too many migrants in subpopulation p" << p_subpop.subpopulation_id_ << "; migration fractions must sum to <= 1.0." << EidosTerminate();
	}
	else
	{
		migration_rates[0] = 1.0;
		migration_sources[0] = &p_subpop;
	}
	
	// SEX ONLY: the sex and asex cases share code but work a bit differently; the sex cases generates females and then males in
	// separate passes, and selfing is disabled in the sex case.  This block sets up for the sex case to diverge in these ways.
	slim_popsize_t total_female_children = 0, total_male_children = 0;
	int number_of_sexes = 1;
	
	if (sex_enabled)
	{
		double sex_ratio = p_subpop.child_sex_ratio_;
		
		total_male_children = static_cast<slim_popsize_t>(lround(total_children * sex_ratio));		// sex ratio is defined as proportion male; round in favor of males, arbitrarily
		total_female_children = total_children - total_male_children;
		number_of_sexes = 2;
		
		if (total_male_children <= 0 || total_female_children <= 0)
			EIDOS_TERMINATION << "ERROR (Population::EvolveSubpopulation): sex ratio " << sex_ratio << " results in a unisexual child population." << EidosTerminate();
	}
	
	if (p_mate_choice_callbacks_present || p_modify_child_callbacks_present || p_recombination_callbacks_present)
	{
		// CALLBACKS PRESENT: We need to generate offspring in a randomized order.  This way the callbacks are presented with potential offspring
		// a random order, and so it is much easier to write a callback that runs for less than the full offspring generation phase (influencing a
		// limited number of mating events, for example).  So in this code branch, we prepare an overall plan for migration and sex, and then execute
		// that plan in an order randomized with gsl_ran_shuffle().  BCH 28 September 2016: When sex is enabled, we want to generate male and female
		// offspring in shuffled order.  However, the vector of child genomes is organized into females first, then males, so we need to fill that
		// vector in an unshuffled order or we end up trying to generate a male offspring into a female slot, or vice versa.  See the usage of
		// child_index_F, child_index_M, and child_index in the shuffle cases below.
		
		if (migrant_source_count == 0)
		{
			// CALLBACKS, NO MIGRATION: Here we are drawing all offspring from the local pool, so we can optimize a lot.  We only need to shuffle
			// the order in which males and females are generated, if we're running with sex, selfing, or cloning; if not, we actually don't need
			// to shuffle at all, because no aspect of the mating algorithm is predetermined.
			slim_popsize_t child_count = 0;	// counter over all subpop_size_ children
			Subpopulation &source_subpop = p_subpop;
			slim_objectid_t subpop_id = source_subpop.subpopulation_id_;
			double selfing_fraction = source_subpop.selfing_fraction_;
			double cloning_fraction = source_subpop.female_clone_fraction_;
			
			// figure out our callback situation for this source subpop; callbacks come from the source, not the destination
			std::vector<SLiMEidosBlock*> *mate_choice_callbacks = nullptr, *modify_child_callbacks = nullptr, *recombination_callbacks = nullptr;
			
			if (p_mate_choice_callbacks_present && source_subpop.registered_mate_choice_callbacks_.size())
				mate_choice_callbacks = &source_subpop.registered_mate_choice_callbacks_;
			if (p_modify_child_callbacks_present && source_subpop.registered_modify_child_callbacks_.size())
				modify_child_callbacks = &source_subpop.registered_modify_child_callbacks_;
			if (p_recombination_callbacks_present && source_subpop.registered_recombination_callbacks_.size())
				recombination_callbacks = &source_subpop.registered_recombination_callbacks_;
			
			if (sex_enabled || (selfing_fraction > 0.0) || (cloning_fraction > 0.0))
			{
				// We have either sex, selfing, or cloning as attributes of each individual child, so we need to pre-plan and shuffle.
				typedef struct
				{
					IndividualSex planned_sex;
					uint8_t planned_cloned;
					uint8_t planned_selfed;
				} offspring_plan;
				
				offspring_plan planned_offspring[total_children];
				
				for (int sex_index = 0; sex_index < number_of_sexes; ++sex_index)
				{
					slim_popsize_t total_children_of_sex;
					IndividualSex child_sex;
					
					if (sex_enabled)
					{
						total_children_of_sex = ((sex_index == 0) ? total_female_children : total_male_children);
						child_sex = ((sex_index == 0) ? IndividualSex::kFemale : IndividualSex::kMale);
					}
					else
					{
						total_children_of_sex = total_children;
						child_sex = IndividualSex::kHermaphrodite;
					}
					
					slim_popsize_t migrants_to_generate = total_children_of_sex;
					
					if (migrants_to_generate > 0)
					{
						// figure out how many from this source subpop are the result of selfing and/or cloning
						slim_popsize_t number_to_self = 0, number_to_clone = 0;
						
						if (selfing_fraction > 0)
						{
							if (cloning_fraction > 0)
							{
								double fractions[3] = {selfing_fraction, cloning_fraction, 1.0 - (selfing_fraction + cloning_fraction)};
								unsigned int counts[3] = {0, 0, 0};
								
								gsl_ran_multinomial(gEidos_rng, 3, (unsigned int)migrants_to_generate, fractions, counts);
								
								number_to_self = static_cast<slim_popsize_t>(counts[0]);
								number_to_clone = static_cast<slim_popsize_t>(counts[1]);
							}
							else
								number_to_self = static_cast<slim_popsize_t>(gsl_ran_binomial(gEidos_rng, selfing_fraction, (unsigned int)migrants_to_generate));
						}
						else if (cloning_fraction > 0)
							number_to_clone = static_cast<slim_popsize_t>(gsl_ran_binomial(gEidos_rng, cloning_fraction, (unsigned int)migrants_to_generate));
						
						// generate all selfed, cloned, and autogamous offspring in one shared loop
						slim_popsize_t migrant_count = 0;
						
						while (migrant_count < migrants_to_generate)
						{
							offspring_plan *offspring_plan_ptr = planned_offspring + child_count;
							
							offspring_plan_ptr->planned_sex = child_sex;
							
							if (number_to_clone > 0)
							{
								offspring_plan_ptr->planned_cloned = true;
								offspring_plan_ptr->planned_selfed = false;
								--number_to_clone;
							}
							else if (number_to_self > 0)
							{
								offspring_plan_ptr->planned_cloned = false;
								offspring_plan_ptr->planned_selfed = true;
								--number_to_self;
							}
							else
							{
								offspring_plan_ptr->planned_cloned = false;
								offspring_plan_ptr->planned_selfed = false;
							}
							
							// change all our counters
							migrant_count++;
							child_count++;
						}
					}
				}
				
				gsl_ran_shuffle(gEidos_rng, planned_offspring, total_children, sizeof(offspring_plan));
				
				// Now we can run through our plan vector and generate each planned child in order.
				slim_popsize_t child_index_F = 0, child_index_M = total_female_children, child_index;
				
				for (child_count = 0; child_count < total_children; ++child_count)
				{
					// Get the plan for this offspring from our shuffled plan vector
					offspring_plan *offspring_plan_ptr = planned_offspring + child_count;
					
					IndividualSex child_sex = offspring_plan_ptr->planned_sex;
					bool selfed, cloned;
					int num_tries = 0;
					
					// Find the appropriate index for the child we are generating; we need to put males and females in the right spots
					if (sex_enabled)
					{
						if (child_sex == IndividualSex::kFemale)
							child_index = child_index_F++;
						else
							child_index = child_index_M++;
					}
					else
					{
						child_index = child_count;
					}
					
					// We loop back to here to retry child generation if a mateChoice() or modifyChild() callback causes our first attempt at
					// child generation to fail.  The first time we generate a given child index, we follow our plan; subsequent times, we
					// draw selfed and cloned randomly based on the probabilities set for the source subpop.  This allows the callbacks to
					// actually influence the proportion selfed/cloned, through e.g. lethal epistatic interactions or failed mate search.
				retryChild:
					
					if (num_tries > 1000000)
						EIDOS_TERMINATION << "ERROR (Population::EvolveSubpopulation): failed to generate child after 1 million attempts; terminating to avoid infinite loop." << EidosTerminate();
					
					if (num_tries == 0)
					{
						// first mating event, so follow our original plan for this offspring
						// note we could draw self/cloned as below even for the first try; this code path is just more efficient,
						// since it avoids a gsl_ran_uniform() for each child, in favor of one gsl_ran_multinomial() above
						selfed = offspring_plan_ptr->planned_selfed;
						cloned = offspring_plan_ptr->planned_cloned;
					}
					else
					{
						// a whole new mating event, so we draw selfed/cloned based on the source subpop's probabilities
						selfed = false;
						cloned = false;
						
						if (selfing_fraction > 0)
						{
							if (cloning_fraction > 0)
							{
								double draw = gsl_rng_uniform(gEidos_rng);
								
								if (draw < selfing_fraction)							selfed = true;
								else if (draw < selfing_fraction + cloning_fraction)	cloned = true;
							}
							else
							{
								double draw = gsl_rng_uniform(gEidos_rng);
								
								if (draw < selfing_fraction)							selfed = true;
							}
						}
						else if (cloning_fraction > 0)
						{
							double draw = gsl_rng_uniform(gEidos_rng);
							
							if (draw < cloning_fraction)								cloned = true;
						}
						
						// we do not redraw the sex of the child, however, because that is predetermined; we need to hit our target ratio
						// we could trade our planned sex for a randomly chosen planned sex from the remaining children to generate, but
						// that gets a little complicated because of selfing, and I'm having trouble imagining a scenario where it matters
					}
					
					slim_popsize_t parent1, parent2;
					
					if (cloned)
					{
						if (sex_enabled)
							parent1 = (child_sex == IndividualSex::kFemale) ? source_subpop.DrawFemaleParentUsingFitness() : source_subpop.DrawMaleParentUsingFitness();
						else
							parent1 = source_subpop.DrawParentUsingFitness();
						
						parent2 = parent1;
						
						DoClonalMutation(&p_subpop, &source_subpop, 2 * child_index, subpop_id, 2 * parent1, p_chromosome, p_generation, child_sex);
						DoClonalMutation(&p_subpop, &source_subpop, 2 * child_index + 1, subpop_id, 2 * parent1 + 1, p_chromosome, p_generation, child_sex);
						
						if (pedigrees_enabled)
							p_subpop.child_individuals_[child_index].TrackPedigreeWithParents(source_subpop.parent_individuals_[parent1], source_subpop.parent_individuals_[parent1]);
					}
					else
					{
						IndividualSex parent1_sex, parent2_sex;
						
						if (sex_enabled)
						{
							parent1 = source_subpop.DrawFemaleParentUsingFitness();
							parent1_sex = IndividualSex::kFemale;
						}
						else
						{
							parent1 = source_subpop.DrawParentUsingFitness();
							parent1_sex = IndividualSex::kHermaphrodite;
						}
						
						if (selfed)
						{
							parent2 = parent1;
							parent2_sex = parent1_sex;
						}
						else if (!mate_choice_callbacks)
						{
							if (sex_enabled)
							{
								parent2 = source_subpop.DrawMaleParentUsingFitness();
								parent2_sex = IndividualSex::kMale;
							}
							else
							{
								do
									parent2 = source_subpop.DrawParentUsingFitness();	// selfing possible!
								while (prevent_incidental_selfing && (parent2 == parent1));
								
								parent2_sex = IndividualSex::kHermaphrodite;
							}
						}
						else
						{
							do
								parent2 = ApplyMateChoiceCallbacks(parent1, &p_subpop, &source_subpop, *mate_choice_callbacks);
							while (prevent_incidental_selfing && (parent2 == parent1));
							
							if (parent2 == -1)
							{
								// The mateChoice() callbacks rejected parent1 altogether, so we need to choose a new parent1 and start over
								num_tries++;
								goto retryChild;
							}
							
							parent2_sex = (sex_enabled ? IndividualSex::kMale : IndividualSex::kHermaphrodite);		// guaranteed by ApplyMateChoiceCallbacks()
						}
						
						// recombination, gene-conversion, mutation
						DoCrossoverMutation(&p_subpop, &source_subpop, 2 * child_index, subpop_id, parent1, p_chromosome, p_generation, child_sex, parent1_sex, recombination_callbacks);
						DoCrossoverMutation(&p_subpop, &source_subpop, 2 * child_index + 1, subpop_id, parent2, p_chromosome, p_generation, child_sex, parent2_sex, recombination_callbacks);
						
						if (pedigrees_enabled)
							p_subpop.child_individuals_[child_index].TrackPedigreeWithParents(source_subpop.parent_individuals_[parent1], source_subpop.parent_individuals_[parent2]);
					}
					
					if (modify_child_callbacks)
					{
						if (!ApplyModifyChildCallbacks(child_index, child_sex, parent1, parent2, selfed, cloned, &p_subpop, &source_subpop, *modify_child_callbacks))
						{
							// The modifyChild() callbacks suppressed the child altogether; this is juvenile migrant mortality, basically, so
							// we need to even change the source subpop for our next attempt.  In this case, however, we have no migration.
							num_tries++;
							goto retryChild;
						}
					}
				}
			}
			else
			{
				// CALLBACKS, NO MIGRATION, NO SEX, NO SELFING, NO CLONING: so we don't need to preplan or shuffle, each child is generated in the same exact way.
				int num_tries = 0;
				
				while (child_count < total_children)
				{
					slim_popsize_t parent1, parent2;
					
					parent1 = source_subpop.DrawParentUsingFitness();
					
					if (!mate_choice_callbacks)
					{
						do
							parent2 = source_subpop.DrawParentUsingFitness();	// selfing possible!
						while (prevent_incidental_selfing && (parent2 == parent1));
					}
					else
					{
						while (true)	// loop while parent2 == -1, indicating a request for a new first parent
						{
							do
								parent2 = ApplyMateChoiceCallbacks(parent1, &p_subpop, &source_subpop, *mate_choice_callbacks);
							while (prevent_incidental_selfing && (parent2 == parent1));
							
							if (parent2 != -1)
								break;
							
							// parent1 was rejected by the callbacks, so we need to redraw a new parent1
							num_tries++;
							parent1 = source_subpop.DrawParentUsingFitness();
							
							if (num_tries > 1000000)
								EIDOS_TERMINATION << "ERROR (Population::EvolveSubpopulation): failed to generate child after 1 million attempts; terminating to avoid infinite loop." << EidosTerminate();
						}
					}
					
					// recombination, gene-conversion, mutation
					DoCrossoverMutation(&p_subpop, &source_subpop, 2 * child_count, subpop_id, parent1, p_chromosome, p_generation, IndividualSex::kHermaphrodite, IndividualSex::kHermaphrodite, recombination_callbacks);
					DoCrossoverMutation(&p_subpop, &source_subpop, 2 * child_count + 1, subpop_id, parent2, p_chromosome, p_generation, IndividualSex::kHermaphrodite, IndividualSex::kHermaphrodite, recombination_callbacks);
					
					if (pedigrees_enabled)
						p_subpop.child_individuals_[child_count].TrackPedigreeWithParents(source_subpop.parent_individuals_[parent1], source_subpop.parent_individuals_[parent2]);
					
					if (modify_child_callbacks)
					{
						if (!ApplyModifyChildCallbacks(child_count, IndividualSex::kHermaphrodite, parent1, parent2, false, false, &p_subpop, &source_subpop, *modify_child_callbacks))
						{
							num_tries++;
							
							if (num_tries > 1000000)
								EIDOS_TERMINATION << "ERROR (Population::EvolveSubpopulation): failed to generate child after 1 million attempts; terminating to avoid infinite loop." << EidosTerminate();
							
							continue;
						}
					}
					
					// if the child was accepted, change all our counters; can't be done before the modifyChild() callback since it might reject the child!
					child_count++;
					num_tries = 0;
				}
			}
		}
		else
		{
			// CALLBACKS WITH MIGRATION: Here we need to shuffle the migration source subpops, as well as the offspring sex.  This makes things so
			// different that it is worth treating it as an entirely separate case; it is far less optimizable than the case without migration.
			// Note that this is pretty much the general case of this whole method; all the other cases are optimized sub-cases of this code!
			slim_popsize_t child_count = 0;	// counter over all subpop_size_ children
			
			// Pre-plan and shuffle.
			typedef struct
			{
				Subpopulation *planned_source;
				IndividualSex planned_sex;
				uint8_t planned_cloned;
				uint8_t planned_selfed;
			} offspring_plan;
			
			offspring_plan planned_offspring[total_children];
			
			for (int sex_index = 0; sex_index < number_of_sexes; ++sex_index)
			{
				slim_popsize_t total_children_of_sex;
				IndividualSex child_sex;
				
				if (sex_enabled)
				{
					total_children_of_sex = ((sex_index == 0) ? total_female_children : total_male_children);
					child_sex = ((sex_index == 0) ? IndividualSex::kFemale : IndividualSex::kMale);
				}
				else
				{
					total_children_of_sex = total_children;
					child_sex = IndividualSex::kHermaphrodite;
				}
				
				// draw the number of individuals from the migrant source subpops, and from ourselves, for the current sex
				if (migrant_source_count == 0)
					num_migrants[0] = (unsigned int)total_children_of_sex;
				else
					gsl_ran_multinomial(gEidos_rng, migrant_source_count + 1, (unsigned int)total_children_of_sex, migration_rates, num_migrants);
				
				// loop over all source subpops, including ourselves
				for (int pop_count = 0; pop_count < migrant_source_count + 1; ++pop_count)
				{
					slim_popsize_t migrants_to_generate = static_cast<slim_popsize_t>(num_migrants[pop_count]);
					
					if (migrants_to_generate > 0)
					{
						Subpopulation &source_subpop = *(migration_sources[pop_count]);
						double selfing_fraction = sex_enabled ? 0.0 : source_subpop.selfing_fraction_;
						double cloning_fraction = (sex_index == 0) ? source_subpop.female_clone_fraction_ : source_subpop.male_clone_fraction_;
						
						// figure out how many from this source subpop are the result of selfing and/or cloning
						slim_popsize_t number_to_self = 0, number_to_clone = 0;
						
						if (selfing_fraction > 0)
						{
							if (cloning_fraction > 0)
							{
								double fractions[3] = {selfing_fraction, cloning_fraction, 1.0 - (selfing_fraction + cloning_fraction)};
								unsigned int counts[3] = {0, 0, 0};
								
								gsl_ran_multinomial(gEidos_rng, 3, (unsigned int)migrants_to_generate, fractions, counts);
								
								number_to_self = static_cast<slim_popsize_t>(counts[0]);
								number_to_clone = static_cast<slim_popsize_t>(counts[1]);
							}
							else
								number_to_self = static_cast<slim_popsize_t>(gsl_ran_binomial(gEidos_rng, selfing_fraction, (unsigned int)migrants_to_generate));
						}
						else if (cloning_fraction > 0)
							number_to_clone = static_cast<slim_popsize_t>(gsl_ran_binomial(gEidos_rng, cloning_fraction, (unsigned int)migrants_to_generate));
						
						// generate all selfed, cloned, and autogamous offspring in one shared loop
						slim_popsize_t migrant_count = 0;
						
						while (migrant_count < migrants_to_generate)
						{
							offspring_plan *offspring_plan_ptr = planned_offspring + child_count;
							
							offspring_plan_ptr->planned_source = &source_subpop;
							offspring_plan_ptr->planned_sex = child_sex;
							
							if (number_to_clone > 0)
							{
								offspring_plan_ptr->planned_cloned = true;
								offspring_plan_ptr->planned_selfed = false;
								--number_to_clone;
							}
							else if (number_to_self > 0)
							{
								offspring_plan_ptr->planned_cloned = false;
								offspring_plan_ptr->planned_selfed = true;
								--number_to_self;
							}
							else
							{
								offspring_plan_ptr->planned_cloned = false;
								offspring_plan_ptr->planned_selfed = false;
							}
							
							// change all our counters
							migrant_count++;
							child_count++;
						}
					}
				}
			}
			
			gsl_ran_shuffle(gEidos_rng, planned_offspring, total_children, sizeof(offspring_plan));
			
			// Now we can run through our plan vector and generate each planned child in order.
			slim_popsize_t child_index_F = 0, child_index_M = total_female_children, child_index;
			
			for (child_count = 0; child_count < total_children; ++child_count)
			{
				// Get the plan for this offspring from our shuffled plan vector
				offspring_plan *offspring_plan_ptr = planned_offspring + child_count;
				
				Subpopulation *source_subpop = offspring_plan_ptr->planned_source;
				IndividualSex child_sex = offspring_plan_ptr->planned_sex;
				bool selfed, cloned;
				int num_tries = 0;
				
				// Find the appropriate index for the child we are generating; we need to put males and females in the right spots
				if (sex_enabled)
				{
					if (child_sex == IndividualSex::kFemale)
						child_index = child_index_F++;
					else
						child_index = child_index_M++;
				}
				else
				{
					child_index = child_count;
				}
				
				// We loop back to here to retry child generation if a modifyChild() callback causes our first attempt at
				// child generation to fail.  The first time we generate a given child index, we follow our plan; subsequent times, we
				// draw selfed and cloned randomly based on the probabilities set for the source subpop.  This allows the callbacks to
				// actually influence the proportion selfed/cloned, through e.g. lethal epistatic interactions or failed mate search.
			retryWithNewSourceSubpop:
				
				slim_objectid_t subpop_id = source_subpop->subpopulation_id_;
				
				// figure out our callback situation for this source subpop; callbacks come from the source, not the destination
				std::vector<SLiMEidosBlock*> *mate_choice_callbacks = nullptr, *modify_child_callbacks = nullptr, *recombination_callbacks = nullptr;
				
				if (source_subpop->registered_mate_choice_callbacks_.size())
					mate_choice_callbacks = &source_subpop->registered_mate_choice_callbacks_;
				if (source_subpop->registered_modify_child_callbacks_.size())
					modify_child_callbacks = &source_subpop->registered_modify_child_callbacks_;
				if (source_subpop->registered_recombination_callbacks_.size())
					recombination_callbacks = &source_subpop->registered_recombination_callbacks_;
				
				// Similar to retryWithNewSourceSubpop: but assumes that the subpop remains unchanged; used after a failed mateChoice()
				// callback, which rejects parent1 but does not cause a redraw of the source subpop.
			retryWithSameSourceSubpop:
				
				if (num_tries > 1000000)
					EIDOS_TERMINATION << "ERROR (Population::EvolveSubpopulation): failed to generate child after 1 million attempts; terminating to avoid infinite loop." << EidosTerminate();
				
				if (num_tries == 0)
				{
					// first mating event, so follow our original plan for this offspring
					// note we could draw self/cloned as below even for the first try; this code path is just more efficient,
					// since it avoids a gsl_ran_uniform() for each child, in favor of one gsl_ran_multinomial() above
					selfed = offspring_plan_ptr->planned_selfed;
					cloned = offspring_plan_ptr->planned_cloned;
				}
				else
				{
					// a whole new mating event, so we draw selfed/cloned based on the source subpop's probabilities
					double selfing_fraction = sex_enabled ? 0.0 : source_subpop->selfing_fraction_;
					double cloning_fraction = (child_sex != IndividualSex::kMale) ? source_subpop->female_clone_fraction_ : source_subpop->male_clone_fraction_;
					
					selfed = false;
					cloned = false;
					
					if (selfing_fraction > 0)
					{
						if (cloning_fraction > 0)
						{
							double draw = gsl_rng_uniform(gEidos_rng);
							
							if (draw < selfing_fraction)							selfed = true;
							else if (draw < selfing_fraction + cloning_fraction)	cloned = true;
						}
						else
						{
							double draw = gsl_rng_uniform(gEidos_rng);
							
							if (draw < selfing_fraction)							selfed = true;
						}
					}
					else if (cloning_fraction > 0)
					{
						double draw = gsl_rng_uniform(gEidos_rng);
						
						if (draw < cloning_fraction)								cloned = true;
					}
					
					// we do not redraw the sex of the child, however, because that is predetermined; we need to hit our target ratio
					// we could trade our planned sex for a randomly chosen planned sex from the remaining children to generate, but
					// that gets a little complicated because of selfing, and I'm having trouble imagining a scenario where it matters
				}
				
				slim_popsize_t parent1, parent2;
				
				if (cloned)
				{
					if (sex_enabled)
						parent1 = (child_sex == IndividualSex::kFemale) ? source_subpop->DrawFemaleParentUsingFitness() : source_subpop->DrawMaleParentUsingFitness();
					else
						parent1 = source_subpop->DrawParentUsingFitness();
					
					parent2 = parent1;
					
					DoClonalMutation(&p_subpop, source_subpop, 2 * child_index, subpop_id, 2 * parent1, p_chromosome, p_generation, child_sex);
					DoClonalMutation(&p_subpop, source_subpop, 2 * child_index + 1, subpop_id, 2 * parent1 + 1, p_chromosome, p_generation, child_sex);
					
					if (pedigrees_enabled)
						p_subpop.child_individuals_[child_index].TrackPedigreeWithParents(source_subpop->parent_individuals_[parent1], source_subpop->parent_individuals_[parent1]);
				}
				else
				{
					IndividualSex parent1_sex, parent2_sex;
					
					if (sex_enabled)
					{
						parent1 = source_subpop->DrawFemaleParentUsingFitness();
						parent1_sex = IndividualSex::kFemale;
					}
					else
					{
						parent1 = source_subpop->DrawParentUsingFitness();
						parent1_sex = IndividualSex::kHermaphrodite;
					}
					
					if (selfed)
					{
						parent2 = parent1;
						parent2_sex = parent1_sex;
					}
					else if (!mate_choice_callbacks)
					{
						if (sex_enabled)
						{
							parent2 = source_subpop->DrawMaleParentUsingFitness();
							parent2_sex = IndividualSex::kMale;
						}
						else
						{
							do
								parent2 = source_subpop->DrawParentUsingFitness();	// selfing possible!
							while (prevent_incidental_selfing && (parent2 == parent1));
							
							parent2_sex = IndividualSex::kHermaphrodite;
						}
					}
					else
					{
						do
							parent2 = ApplyMateChoiceCallbacks(parent1, &p_subpop, source_subpop, *mate_choice_callbacks);
						while (prevent_incidental_selfing && (parent2 == parent1));
						
						if (parent2 == -1)
						{
							// The mateChoice() callbacks rejected parent1 altogether, so we need to choose a new parent1 and start over
							num_tries++;
							goto retryWithSameSourceSubpop;
						}
						
						parent2_sex = (sex_enabled ? IndividualSex::kMale : IndividualSex::kHermaphrodite);		// guaranteed by ApplyMateChoiceCallbacks()
					}
					
					// recombination, gene-conversion, mutation
					DoCrossoverMutation(&p_subpop, source_subpop, 2 * child_index, subpop_id, parent1, p_chromosome, p_generation, child_sex, parent1_sex, recombination_callbacks);
					DoCrossoverMutation(&p_subpop, source_subpop, 2 * child_index + 1, subpop_id, parent2, p_chromosome, p_generation, child_sex, parent2_sex, recombination_callbacks);
					
					if (pedigrees_enabled)
						p_subpop.child_individuals_[child_index].TrackPedigreeWithParents(source_subpop->parent_individuals_[parent1], source_subpop->parent_individuals_[parent2]);
				}
				
				if (modify_child_callbacks)
				{
					if (!ApplyModifyChildCallbacks(child_index, child_sex, parent1, parent2, selfed, cloned, &p_subpop, source_subpop, *modify_child_callbacks))
					{
						// The modifyChild() callbacks suppressed the child altogether; this is juvenile migrant mortality, basically, so
						// we need to even change the source subpop for our next attempt, so that differential mortality between different
						// migration sources leads to differential representation in the offspring generation – more offspring from the
						// subpop that is more successful at contributing migrants.
						gsl_ran_multinomial(gEidos_rng, migrant_source_count + 1, 1, migration_rates, num_migrants);
						
						for (int pop_count = 0; pop_count < migrant_source_count + 1; ++pop_count)
							if (num_migrants[pop_count] > 0)
							{
								source_subpop = migration_sources[pop_count];
								break;
							}
						
						num_tries++;
						goto retryWithNewSourceSubpop;
					}
				}
			}
		}
	}
	else
	{
		// NO CALLBACKS PRESENT: offspring can be generated in a fixed (i.e. predetermined) order.  This is substantially faster, since it avoids
		// some setup overhead, including the gsl_ran_shuffle() call.  All code that accesses individuals within a subpopulation needs to be aware of
		// the fact that the individuals might be in a non-random order, because of this code path.  BEWARE!
		
		// We loop to generate females first (sex_index == 0) and males second (sex_index == 1).
		// In nonsexual simulations number_of_sexes == 1 and this loops just once.
		slim_popsize_t child_count = 0;	// counter over all subpop_size_ children
		
		for (int sex_index = 0; sex_index < number_of_sexes; ++sex_index)
		{
			slim_popsize_t total_children_of_sex;
			IndividualSex child_sex;
			
			if (sex_enabled)
			{
				total_children_of_sex = ((sex_index == 0) ? total_female_children : total_male_children);
				child_sex = ((sex_index == 0) ? IndividualSex::kFemale : IndividualSex::kMale);
			}
			else
			{
				total_children_of_sex = total_children;
				child_sex = IndividualSex::kHermaphrodite;
			}
			
			// draw the number of individuals from the migrant source subpops, and from ourselves, for the current sex
			if (migrant_source_count == 0)
				num_migrants[0] = (unsigned int)total_children_of_sex;
			else
				gsl_ran_multinomial(gEidos_rng, migrant_source_count + 1, (unsigned int)total_children_of_sex, migration_rates, num_migrants);
			
			// loop over all source subpops, including ourselves
			for (int pop_count = 0; pop_count < migrant_source_count + 1; ++pop_count)
			{
				slim_popsize_t migrants_to_generate = static_cast<slim_popsize_t>(num_migrants[pop_count]);
				
				if (migrants_to_generate > 0)
				{
					Subpopulation &source_subpop = *(migration_sources[pop_count]);
					slim_objectid_t subpop_id = source_subpop.subpopulation_id_;
					double selfing_fraction = sex_enabled ? 0.0 : source_subpop.selfing_fraction_;
					double cloning_fraction = (sex_index == 0) ? source_subpop.female_clone_fraction_ : source_subpop.male_clone_fraction_;
					
					// figure out how many from this source subpop are the result of selfing and/or cloning
					slim_popsize_t number_to_self = 0, number_to_clone = 0;
					
					if (selfing_fraction > 0)
					{
						if (cloning_fraction > 0)
						{
							double fractions[3] = {selfing_fraction, cloning_fraction, 1.0 - (selfing_fraction + cloning_fraction)};
							unsigned int counts[3] = {0, 0, 0};
							
							gsl_ran_multinomial(gEidos_rng, 3, (unsigned int)migrants_to_generate, fractions, counts);
							
							number_to_self = static_cast<slim_popsize_t>(counts[0]);
							number_to_clone = static_cast<slim_popsize_t>(counts[1]);
						}
						else
							number_to_self = static_cast<slim_popsize_t>(gsl_ran_binomial(gEidos_rng, selfing_fraction, (unsigned int)migrants_to_generate));
					}
					else if (cloning_fraction > 0)
						number_to_clone = static_cast<slim_popsize_t>(gsl_ran_binomial(gEidos_rng, cloning_fraction, (unsigned int)migrants_to_generate));
					
					// generate all selfed, cloned, and autogamous offspring in one shared loop
					slim_popsize_t migrant_count = 0;
					
					if ((number_to_self == 0) && (number_to_clone == 0))
					{
						// a simple loop for the base case with no selfing, no cloning, and no callbacks; we split into two cases by sex_enabled for maximal speed
						if (sex_enabled)
						{
							while (migrant_count < migrants_to_generate)
							{
								slim_popsize_t parent1 = source_subpop.DrawFemaleParentUsingFitness();
								slim_popsize_t parent2 = source_subpop.DrawMaleParentUsingFitness();
								
								// recombination, gene-conversion, mutation
								DoCrossoverMutation(&p_subpop, &source_subpop, 2 * child_count, subpop_id, parent1, p_chromosome, p_generation, child_sex, IndividualSex::kFemale, nullptr);
								DoCrossoverMutation(&p_subpop, &source_subpop, 2 * child_count + 1, subpop_id, parent2, p_chromosome, p_generation, child_sex, IndividualSex::kMale, nullptr);
								
								if (pedigrees_enabled)
									p_subpop.child_individuals_[child_count].TrackPedigreeWithParents(source_subpop.parent_individuals_[parent1], source_subpop.parent_individuals_[parent2]);
								
								migrant_count++;
								child_count++;
							}
						}
						else
						{
							while (migrant_count < migrants_to_generate)
							{
								slim_popsize_t parent1 = source_subpop.DrawParentUsingFitness();
								slim_popsize_t parent2;
								
								do
									parent2 = source_subpop.DrawParentUsingFitness();	// note this does not prohibit selfing!
								while (prevent_incidental_selfing && (parent2 == parent1));
								
								// recombination, gene-conversion, mutation
								DoCrossoverMutation(&p_subpop, &source_subpop, 2 * child_count, subpop_id, parent1, p_chromosome, p_generation, child_sex, IndividualSex::kHermaphrodite, nullptr);
								DoCrossoverMutation(&p_subpop, &source_subpop, 2 * child_count + 1, subpop_id, parent2, p_chromosome, p_generation, child_sex, IndividualSex::kHermaphrodite, nullptr);
								
								if (pedigrees_enabled)
									p_subpop.child_individuals_[child_count].TrackPedigreeWithParents(source_subpop.parent_individuals_[parent1], source_subpop.parent_individuals_[parent2]);
								
								migrant_count++;
								child_count++;
							}
						}
					}
					else
					{
						// the full loop with support for selfing/cloning (but no callbacks, since we're in that overall branch)
						while (migrant_count < migrants_to_generate)
						{
							slim_popsize_t parent1, parent2;
							
							if (number_to_clone > 0)
							{
								if (sex_enabled)
									parent1 = (child_sex == IndividualSex::kFemale) ? source_subpop.DrawFemaleParentUsingFitness() : source_subpop.DrawMaleParentUsingFitness();
								else
									parent1 = source_subpop.DrawParentUsingFitness();
								
								parent2 = parent1;
								(void)parent2;		// tell the static analyzer that we know we just did a dead store
								
								--number_to_clone;
								
								DoClonalMutation(&p_subpop, &source_subpop, 2 * child_count, subpop_id, 2 * parent1, p_chromosome, p_generation, child_sex);
								DoClonalMutation(&p_subpop, &source_subpop, 2 * child_count + 1, subpop_id, 2 * parent1 + 1, p_chromosome, p_generation, child_sex);
								
								if (pedigrees_enabled)
									p_subpop.child_individuals_[child_count].TrackPedigreeWithParents(source_subpop.parent_individuals_[parent1], source_subpop.parent_individuals_[parent1]);
							}
							else
							{
								IndividualSex parent1_sex, parent2_sex;
								
								if (sex_enabled)
								{
									parent1 = source_subpop.DrawFemaleParentUsingFitness();
									parent1_sex = IndividualSex::kFemale;
								}
								else
								{
									parent1 = source_subpop.DrawParentUsingFitness();
									parent1_sex = IndividualSex::kHermaphrodite;
								}
								
								if (number_to_self > 0)
								{
									parent2 = parent1;
									parent2_sex = parent1_sex;
									--number_to_self;
								}
								else
								{
									if (sex_enabled)
									{
										parent2 = source_subpop.DrawMaleParentUsingFitness();
										parent2_sex = IndividualSex::kMale;
									}
									else
									{
										do
											parent2 = source_subpop.DrawParentUsingFitness();	// selfing possible!
										while (prevent_incidental_selfing && (parent2 == parent1));
										
										parent2_sex = IndividualSex::kHermaphrodite;
									}
								}
								
								// recombination, gene-conversion, mutation
								DoCrossoverMutation(&p_subpop, &source_subpop, 2 * child_count, subpop_id, parent1, p_chromosome, p_generation, child_sex, parent1_sex, nullptr);
								DoCrossoverMutation(&p_subpop, &source_subpop, 2 * child_count + 1, subpop_id, parent2, p_chromosome, p_generation, child_sex, parent2_sex, nullptr);
								
								if (pedigrees_enabled)
									p_subpop.child_individuals_[child_count].TrackPedigreeWithParents(source_subpop.parent_individuals_[parent1], source_subpop.parent_individuals_[parent2]);
							}
							
							// change counters
							migrant_count++;
							child_count++;
						}
					}
				}
			}
		}
	}
}

// apply recombination() callbacks to a generated child; a return of true means breakpoints were changed
bool Population::ApplyRecombinationCallbacks(slim_popsize_t p_parent_index, Genome *p_genome1, Genome *p_genome2, Subpopulation *p_source_subpop, std::vector<slim_position_t> &p_crossovers, std::vector<slim_position_t> &p_gc_starts, std::vector<slim_position_t> &p_gc_ends, std::vector<SLiMEidosBlock*> &p_recombination_callbacks)
{
#if defined(SLIMGUI) && (SLIMPROFILING == 1)
	// PROFILING
	SLIM_PROFILE_BLOCK_START();
#endif
	
	bool crossovers_changed = false, gcstarts_changed = false, gcends_changed = false;
	EidosValue_SP local_crossovers_ptr, local_gcstarts_ptr, local_gcends_ptr;
	
	for (SLiMEidosBlock *recombination_callback : p_recombination_callbacks)
	{
		if (recombination_callback->active_)
		{
			// The callback is active, so we need to execute it
			EidosSymbolTable callback_symbols(EidosSymbolTableType::kContextConstantsTable, &sim_.SymbolTable());
			EidosSymbolTable client_symbols(EidosSymbolTableType::kVariablesTable, &callback_symbols);
			EidosFunctionMap &function_map = sim_.FunctionMap();
			EidosInterpreter interpreter(recombination_callback->compound_statement_node_, client_symbols, function_map, &sim_);
			
			if (recombination_callback->contains_self_)
				callback_symbols.InitializeConstantSymbolEntry(recombination_callback->SelfSymbolTableEntry());		// define "self"
			
			// Set all of the callback's parameters; note we use InitializeConstantSymbolEntry() for speed.
			// We can use that method because we know the lifetime of the symbol table is shorter than that of
			// the value objects, and we know that the values we are setting here will not change (the objects
			// referred to by the values may change, but the values themselves will not change).
			if (recombination_callback->contains_individual_)
			{
				Individual *individual = &(p_source_subpop->parent_individuals_[p_parent_index]);
				callback_symbols.InitializeConstantSymbolEntry(gID_individual, individual->CachedEidosValue());
			}
			if (recombination_callback->contains_genome1_)
				callback_symbols.InitializeConstantSymbolEntry(gID_genome1, p_genome1->CachedEidosValue());
			if (recombination_callback->contains_genome2_)
				callback_symbols.InitializeConstantSymbolEntry(gID_genome2, p_genome2->CachedEidosValue());
			if (recombination_callback->contains_subpop_)
				callback_symbols.InitializeConstantSymbolEntry(gID_subpop, p_source_subpop->SymbolTableEntry().second);
			
			// All the variable entries for the crossovers and gene conversion start/end points
			if (recombination_callback->contains_breakpoints_)
			{
				if (!local_crossovers_ptr)
					local_crossovers_ptr = EidosValue_SP(new (gEidosValuePool->AllocateChunk()) EidosValue_Int_vector(p_crossovers));
				client_symbols.SetValueForSymbolNoCopy(gID_breakpoints, local_crossovers_ptr);
			}
			if (recombination_callback->contains_gcStarts_)
			{
				if (!local_gcstarts_ptr)
					local_gcstarts_ptr = EidosValue_SP(new (gEidosValuePool->AllocateChunk()) EidosValue_Int_vector(p_gc_starts));
				client_symbols.SetValueForSymbolNoCopy(gID_gcStarts, local_gcstarts_ptr);
			}
			if (recombination_callback->contains_gcEnds_)
			{
				if (!local_gcends_ptr)
					local_gcends_ptr = EidosValue_SP(new (gEidosValuePool->AllocateChunk()) EidosValue_Int_vector(p_gc_ends));
				client_symbols.SetValueForSymbolNoCopy(gID_gcEnds, local_gcends_ptr);
			}
			
			try
			{
				// Interpret the script; the result from the interpretation must be a singleton logical, T if breakpoints have been changed, F otherwise
				EidosValue_SP result_SP = interpreter.EvaluateInternalBlock(recombination_callback->script_);
				EidosValue *result = result_SP.get();
				
				if ((result->Type() != EidosValueType::kValueLogical) || (result->Count() != 1))
					EIDOS_TERMINATION << "ERROR (Population::ApplyRecombinationCallbacks): recombination() callbacks must provide a logical singleton return value." << EidosTerminate(recombination_callback->identifier_token_);
				
				eidos_logical_t breakpoints_changed = result->LogicalAtIndex(0, nullptr);
				
				// If the callback says that breakpoints were changed, check for an actual change in value for the variables referenced by the callback
				if (breakpoints_changed)
				{
					if (recombination_callback->contains_breakpoints_)
					{
						EidosValue_SP new_crossovers = client_symbols.GetValueOrRaiseForSymbol(gID_breakpoints);
						
						if (new_crossovers != local_crossovers_ptr)
						{
							if (new_crossovers->Type() != EidosValueType::kValueInt)
								EIDOS_TERMINATION << "ERROR (Population::ApplyRecombinationCallbacks): recombination() callbacks must provide output values (breakpoints) of type integer." << EidosTerminate(recombination_callback->identifier_token_);
							
							new_crossovers.swap(local_crossovers_ptr);
							crossovers_changed = true;
						}
					}
					if (recombination_callback->contains_gcStarts_)
					{
						EidosValue_SP new_gcstarts = client_symbols.GetValueOrRaiseForSymbol(gID_gcStarts);
						
						if (new_gcstarts != local_gcstarts_ptr)
						{
							if (new_gcstarts->Type() != EidosValueType::kValueInt)
								EIDOS_TERMINATION << "ERROR (Population::ApplyRecombinationCallbacks): recombination() callbacks must provide output values (gcStarts) of type integer." << EidosTerminate(recombination_callback->identifier_token_);
							
							new_gcstarts.swap(local_gcstarts_ptr);
							gcstarts_changed = true;
						}
					}
					if (recombination_callback->contains_gcEnds_)
					{
						EidosValue_SP new_gcends = client_symbols.GetValueOrRaiseForSymbol(gID_gcEnds);
						
						if (new_gcends != local_gcends_ptr)
						{
							if (new_gcends->Type() != EidosValueType::kValueInt)
								EIDOS_TERMINATION << "ERROR (Population::ApplyRecombinationCallbacks): recombination() callbacks must provide output values (gcEnds) of type integer." << EidosTerminate(recombination_callback->identifier_token_);
							
							new_gcends.swap(local_gcends_ptr);
							gcends_changed = true;
						}
					}
				}
				
				// Output generated by the interpreter goes to our output stream
				SLIM_OUTSTREAM << interpreter.ExecutionOutput();
			}
			catch (...)
			{
				// Emit final output even on a throw, so that stop() messages and such get printed
				SLIM_OUTSTREAM << interpreter.ExecutionOutput();
				
				throw;
			}
		}
	}
	
	// Read out the final values of breakpoint vectors that changed
	bool breakpoints_changed = false;
	
	if (crossovers_changed)
	{
		int count = local_crossovers_ptr->Count();
		
		p_crossovers.resize(count);		// zero-fills only new entries at the margin, so is minimally wasteful
		
		if (count == 1)
			p_crossovers[0] = (slim_position_t)local_crossovers_ptr->IntAtIndex(0, nullptr);
		else
		{
			const EidosValue_Int_vector *new_crossover_vector = local_crossovers_ptr->IntVector();
			const int64_t *new_crossover_data = new_crossover_vector->data();
			slim_position_t *p_crossovers_data = p_crossovers.data();
			
			for (int value_index = 0; value_index < count; ++value_index)
				p_crossovers_data[value_index] = (slim_position_t)new_crossover_data[value_index];
		}
		
		breakpoints_changed = true;
	}
	
	if (gcstarts_changed)
	{
		int count = local_gcstarts_ptr->Count();
		
		p_gc_starts.resize(count);		// zero-fills only new entries at the margin, so is minimally wasteful
		
		if (count == 1)
			p_gc_starts[0] = (slim_position_t)local_gcstarts_ptr->IntAtIndex(0, nullptr);
		else
		{
			const EidosValue_Int_vector *new_gcstarts_vector = local_gcstarts_ptr->IntVector();
			const int64_t *new_gcstarts_data = new_gcstarts_vector->data();
			slim_position_t *p_gc_starts_data = p_gc_starts.data();
			
			for (int value_index = 0; value_index < count; ++value_index)
				p_gc_starts_data[value_index] = (slim_position_t)new_gcstarts_data[value_index];
		}
		
		breakpoints_changed = true;
	}
	
	if (gcends_changed)
	{
		int count = local_gcends_ptr->Count();
		
		p_gc_ends.resize(count);		// zero-fills only new entries at the margin, so is minimally wasteful
		
		if (count == 1)
			p_gc_ends[0] = (slim_position_t)local_gcends_ptr->IntAtIndex(0, nullptr);
		else
		{
			const EidosValue_Int_vector *new_gcends_vector = local_gcends_ptr->IntVector();
			const int64_t *new_gcends_data = new_gcends_vector->data();
			slim_position_t *p_gc_ends_data = p_gc_ends.data();
			
			for (int value_index = 0; value_index < count; ++value_index)
				p_gc_ends_data[value_index] = (slim_position_t)new_gcends_data[value_index];
		}
		
		breakpoints_changed = true;
	}
	
#if defined(SLIMGUI) && (SLIMPROFILING == 1)
	// PROFILING
	SLIM_PROFILE_BLOCK_END(sim_.profile_callback_totals_[(int)(SLiMEidosBlockType::SLiMEidosRecombinationCallback)]);
#endif
	
	return breakpoints_changed;
}

// generate a child genome from parental genomes, with recombination, gene conversion, and mutation
void Population::DoCrossoverMutation(Subpopulation *p_subpop, Subpopulation *p_source_subpop, slim_popsize_t p_child_genome_index, slim_objectid_t p_source_subpop_id, slim_popsize_t p_parent_index, const Chromosome &p_chromosome, slim_generation_t p_generation, IndividualSex p_child_sex, IndividualSex p_parent_sex, std::vector<SLiMEidosBlock*> *p_recombination_callbacks)
{
	slim_popsize_t parent_genome_1_index = p_parent_index * 2;
	slim_popsize_t parent_genome_2_index = parent_genome_1_index + 1;
	
	// child genome p_child_genome_index in subpopulation p_subpop_id is assigned outcome of cross-overs at breakpoints in all_breakpoints
	// between parent genomes p_parent1_genome_index and p_parent2_genome_index from subpopulation p_source_subpop_id and new mutations added
	// 
	// example: all_breakpoints = (r1, r2)
	// 
	// mutations (      x < r1) assigned from p1
	// mutations (r1 <= x < r2) assigned from p2
	// mutations (r2 <= x     ) assigned from p1
	
	// A lot of the checks here are only on when DEBUG is defined.  They should absolutely never be hit; if they are, it indicates a flaw
	// in SLiM's internal logic, not user error.  This method gets called a whole lot; every test makes a speed difference.  So disabling
	// these checks seems to make sense.  Of course, if you want the checks on, just define DEBUG.
	
#ifdef DEBUG
	if (p_child_sex == IndividualSex::kUnspecified)
		EIDOS_TERMINATION << "ERROR (Population::DoCrossoverMutation): Child sex cannot be IndividualSex::kUnspecified." << EidosTerminate();
#endif
	
	bool use_only_strand_1 = false;		// if true, we are in a case where crossover cannot occur, and we are to use only parent strand 1
	bool do_swap = true;				// if true, we are to swap the parental strands at the beginning, either 50% of the time (if use_only_strand_1 is false), or always (if use_only_strand_1 is true – in other words, we are directed to use only strand 2)
	
	Genome &child_genome = p_subpop->child_genomes_[p_child_genome_index];
	GenomeType child_genome_type = child_genome.Type();
	Genome *parent_genome_1 = &(p_source_subpop->parent_genomes_[parent_genome_1_index]);
	GenomeType parent1_genome_type = parent_genome_1->Type();
	Genome *parent_genome_2 = &(p_source_subpop->parent_genomes_[parent_genome_2_index]);
	GenomeType parent2_genome_type = parent_genome_2->Type();
	
	if (child_genome_type == GenomeType::kAutosome)
	{
		// If we're modeling autosomes, we can disregard p_child_sex entirely; we don't care whether we're modeling sexual or hermaphrodite individuals
#ifdef DEBUG
		if (parent1_genome_type != GenomeType::kAutosome || parent2_genome_type != GenomeType::kAutosome)
			EIDOS_TERMINATION << "ERROR (Population::DoCrossoverMutation): Mismatch between parent and child genome types (case 1)." << EidosTerminate();
#endif
	}
	else
	{
		// SEX ONLY: If we're modeling sexual individuals, then there are various degenerate cases to be considered, since X and Y don't cross over, there are null chromosomes, etc.
#ifdef DEBUG
		if (p_child_sex == IndividualSex::kHermaphrodite)
			EIDOS_TERMINATION << "ERROR (Population::DoCrossoverMutation): A hermaphrodite child is requested but the child genome is not autosomal." << EidosTerminate();
		if (parent1_genome_type == GenomeType::kAutosome || parent2_genome_type == GenomeType::kAutosome)
			EIDOS_TERMINATION << "ERROR (Population::DoCrossoverMutation): Mismatch between parent and child genome types (case 2)." << EidosTerminate();
#endif
		
		if (child_genome_type == GenomeType::kXChromosome)
		{
			if (p_child_sex == IndividualSex::kMale)
			{
				// If our parent is male (XY or YX), then we have a mismatch, because we're supposed to be male and we're supposed to be getting an X chromosome, but the X must come from the female
				if (parent1_genome_type == GenomeType::kYChromosome || parent2_genome_type == GenomeType::kYChromosome)
					EIDOS_TERMINATION << "ERROR (Population::DoCrossoverMutation): Mismatch between parent and child genome types (case 3)." << EidosTerminate();
				
				// else: we're doing inheritance from the female (XX) to get our X chromosome; we treat this just like the autosomal case
			}
			else if (p_child_sex == IndividualSex::kFemale)
			{
				if (parent1_genome_type == GenomeType::kYChromosome && parent2_genome_type == GenomeType::kXChromosome)
				{
					// we're doing inheritance from the male (YX) to get an X chromosome; we need to ensure that we take the X
					use_only_strand_1 = true; do_swap = true;	// use strand 2
				}
				else if (parent1_genome_type == GenomeType::kXChromosome && parent2_genome_type == GenomeType::kYChromosome)
				{
					// we're doing inheritance from the male (XY) to get an X chromosome; we need to ensure that we take the X
					use_only_strand_1 = true; do_swap = false;	// use strand 1
				}
				// else: we're doing inheritance from the female (XX) to get an X chromosome; we treat this just like the autosomal case
			}
		}
		else // (child_genome_type == GenomeType::kYChromosome), so p_child_sex == IndividualSex::kMale
		{
			if (p_child_sex == IndividualSex::kFemale)
				EIDOS_TERMINATION << "ERROR (Population::DoCrossoverMutation): A female child is requested but the child genome is a Y chromosome." << EidosTerminate();
			
			if (parent1_genome_type == GenomeType::kYChromosome && parent2_genome_type == GenomeType::kXChromosome)
			{
				// we're doing inheritance from the male (YX) to get a Y chromosome; we need to ensure that we take the Y
				use_only_strand_1 = true; do_swap = false;	// use strand 1
			}
			else if (parent1_genome_type == GenomeType::kXChromosome && parent2_genome_type == GenomeType::kYChromosome)
			{
				// we're doing inheritance from the male (XY) to get an X chromosome; we need to ensure that we take the Y
				use_only_strand_1 = true; do_swap = true;	// use strand 2
			}
			else
			{
				// else: we're doing inheritance from the female (XX) to get a Y chromosome, so this is a mismatch
				EIDOS_TERMINATION << "ERROR (Population::DoCrossoverMutation): Mismatch between parent and child genome types (case 4)." << EidosTerminate();
			}
		}
	}
	
	// swap strands in half of cases to assure random assortment (or in all cases, if use_only_strand_1 == true, meaning that crossover cannot occur)
	if (do_swap && (use_only_strand_1 || Eidos_RandomBool(gEidos_rng)))
	{
		std::swap(parent_genome_1_index, parent_genome_2_index);
		std::swap(parent_genome_1, parent_genome_2);
		//std::swap(parent1_genome_type, parent2_genome_type);		// Not used below this point...
	}
	
	// check for null cases
	bool child_genome_null = child_genome.IsNull();
#ifdef DEBUG
	bool parent_genome_1_null = parent_genome_1->IsNull();
	bool parent_genome_2_null = parent_genome_2->IsNull();
#endif
	
	if (child_genome_null)
	{
#ifdef DEBUG
		if (!use_only_strand_1)
		{
			// If we're trying to cross over, both parental strands had better be null
			if (!parent_genome_1_null || !parent_genome_2_null)
				EIDOS_TERMINATION << "ERROR (Population::DoCrossoverMutation): Child genome is null, but crossover is requested and a parental genome is non-null." << EidosTerminate();
		}
		else
		{
			// So we are not crossing over, and we are supposed to use strand 1; it should also be null, otherwise something has gone wrong
			if (!parent_genome_1_null)
				EIDOS_TERMINATION << "ERROR (Population::DoCrossoverMutation): Child genome is null, but the parental strand is not." << EidosTerminate();
		}
#endif
		
		// a null strand cannot cross over and cannot mutate, so we are done
		return;
	}
	
#ifdef DEBUG
	if (use_only_strand_1 && parent_genome_1_null)
		EIDOS_TERMINATION << "ERROR (Population::DoCrossoverMutation): Child genome is non-null, but the parental strand is null." << EidosTerminate();
	
	if (!use_only_strand_1 && (parent_genome_1_null || parent_genome_2_null))
		EIDOS_TERMINATION << "ERROR (Population::DoCrossoverMutation): Child genome is non-null, but a parental strand is null." << EidosTerminate();
#endif
	
	//
	//	OK!  We should have covered all error cases above, so we can now proceed with more alacrity.  We just need to follow
	//	the instructions given to us from above, namely use_only_strand_1.  We know we are doing a non-null strand.
	//
	
	// determine how many mutations and breakpoints we have
	int num_mutations, num_breakpoints;
	static std::vector<slim_position_t> all_breakpoints;	// avoid buffer reallocs, etc.
	
	all_breakpoints.clear();
	
	if (use_only_strand_1)
	{
		num_breakpoints = 0;
		num_mutations = p_chromosome.DrawMutationCount(p_parent_sex);
		
		// no call to recombination() callbacks here, since recombination is not possible
		
		// Note that we do not add the (p_chromosome.last_position_mutrun_ + 1) breakpoint here, for speed in the
		// cases where it is not needed; this needs to be patched up below in the cases where it *is* needed
	}
	else
	{
#ifdef USE_GSL_POISSON
		// When using the GSL's poisson draw, we have to draw the mutation count and breakpoint count separately;
		// the DrawMutationAndBreakpointCounts() method does not support USE_GSL_POISSON
		num_mutations = p_chromosome.DrawMutationCount(p_parent_sex);
		num_breakpoints = p_chromosome.DrawBreakpointCount(p_parent_sex);
#else
		// get both the number of mutations and the number of breakpoints here; this allows us to draw both jointly, super fast!
		p_chromosome.DrawMutationAndBreakpointCounts(p_parent_sex, &num_mutations, &num_breakpoints);
#endif
		
		//std::cout << num_mutations << " mutations, " << num_breakpoints << " breakpoints" << std::endl;
		
		// handle recombination() breakpoints here, if any
		if (p_recombination_callbacks)
		{
			// first draw breakpoints, distinguing between crossovers and gene conversion start/end points
			std::vector<slim_position_t> crossovers, gc_starts, gc_ends;
			
			if (num_breakpoints)
				p_chromosome.DrawBreakpoints_Detailed(p_parent_sex, num_breakpoints, crossovers, gc_starts, gc_ends);
			
			// next, apply the recombination callbacks
			ApplyRecombinationCallbacks(p_parent_index, parent_genome_1, parent_genome_2, p_source_subpop, crossovers, gc_starts, gc_ends, *p_recombination_callbacks);
			
			// Finally, combine the crossovers and gene conversion start/end points into a single sorted, uniqued vector
			num_breakpoints = (int)(crossovers.size() + gc_starts.size() + gc_ends.size());
			
			if (num_breakpoints)
			{
				all_breakpoints.insert(all_breakpoints.end(), crossovers.begin(), crossovers.end());
				all_breakpoints.insert(all_breakpoints.end(), gc_starts.begin(), gc_starts.end());
				all_breakpoints.insert(all_breakpoints.end(), gc_ends.begin(), gc_ends.end());
				all_breakpoints.emplace_back(p_chromosome.last_position_mutrun_ + 1);
				
				std::sort(all_breakpoints.begin(), all_breakpoints.end());
				all_breakpoints.erase(unique(all_breakpoints.begin(), all_breakpoints.end()), all_breakpoints.end());
			}
			else
			{
				// Note that we do not add the (p_chromosome.last_position_mutrun_ + 1) breakpoint here, for speed in the
				// cases where it is not needed; this needs to be patched up below in the cases where it *is* needed
			}
		}
		else if (num_breakpoints)
		{
			// just draw, sort, and unique breakpoints in the standard way
			p_chromosome.DrawBreakpoints(p_parent_sex, num_breakpoints, all_breakpoints);
			
			all_breakpoints.emplace_back(p_chromosome.last_position_mutrun_ + 1);
			std::sort(all_breakpoints.begin(), all_breakpoints.end());
			all_breakpoints.erase(unique(all_breakpoints.begin(), all_breakpoints.end()), all_breakpoints.end());
		}
		else
		{
			// Note that we do not add the (p_chromosome.last_position_mutrun_ + 1) breakpoint here, for speed in the
			// cases where it is not needed; this needs to be patched up below in the cases where it *is* needed
		}
	}
	
	// mutations are usually rare, so let's streamline the case where none occur
	if (num_mutations == 0)
	{
		if (num_breakpoints == 0)
		{
			//
			// no mutations and no crossovers, so the child genome is just a copy of the parental genome
			//
			
			child_genome.copy_from_genome(*parent_genome_1);
		}
		else
		{
			//
			// no mutations, but we do have crossovers, so we just need to interleave the two parental genomes
			//
			
			// start with a clean slate in the child genome
			child_genome.clear_to_nullptr();
			
			Mutation *mut_block_ptr = gSLiM_Mutation_Block;
			Genome *parent_genome = parent_genome_1;
			int mutrun_length = child_genome.mutrun_length_;
			int mutrun_count = child_genome.mutrun_count_;
			int first_uncompleted_mutrun = 0;
			int break_index_max = static_cast<int>(all_breakpoints.size());	// can be != num_breakpoints+1 due to gene conversion and dup removal!
			
			for (int break_index = 0; break_index < break_index_max; break_index++)
			{
				slim_position_t breakpoint = all_breakpoints[break_index];
				int break_mutrun_index = breakpoint / mutrun_length;
				
				// Copy over mutation runs until we arrive at the run in which the breakpoint occurs
				while (break_mutrun_index > first_uncompleted_mutrun)
				{
					child_genome.mutruns_[first_uncompleted_mutrun] = parent_genome->mutruns_[first_uncompleted_mutrun];
					++first_uncompleted_mutrun;
					
					if (first_uncompleted_mutrun >= mutrun_count)
						break;
				}
				
				// Now we are supposed to process a breakpoint in first_uncompleted_mutrun; check whether that means we're done
				if (first_uncompleted_mutrun >= mutrun_count)
					break;
				
				// The break occurs to the left of the base position of the breakpoint; check whether that is between runs
				if (breakpoint > break_mutrun_index * mutrun_length)
				{
					// The breakpoint occurs *inside* the run, so process the run by copying mutations and switching strands
					int this_mutrun_index = first_uncompleted_mutrun;
					const MutationIndex *parent1_iter		= parent_genome_1->mutruns_[this_mutrun_index]->begin_pointer_const();
					const MutationIndex *parent2_iter		= parent_genome_2->mutruns_[this_mutrun_index]->begin_pointer_const();
					const MutationIndex *parent1_iter_max	= parent_genome_1->mutruns_[this_mutrun_index]->end_pointer_const();
					const MutationIndex *parent2_iter_max	= parent_genome_2->mutruns_[this_mutrun_index]->end_pointer_const();
					const MutationIndex *parent_iter		= parent1_iter;
					const MutationIndex *parent_iter_max	= parent1_iter_max;
					MutationRun *child_mutrun = child_genome.WillCreateRun(this_mutrun_index);
					
					while (true)
					{
						// while there are still old mutations in the parent before the current breakpoint...
						while (parent_iter != parent_iter_max)
						{
							MutationIndex current_mutation = *parent_iter;
							
							if ((mut_block_ptr + current_mutation)->position_ >= breakpoint)
								break;
							
							// add the old mutation; no need to check for a duplicate here since the parental genome is already duplicate-free
							child_mutrun->emplace_back(current_mutation);
							
							parent_iter++;
						}
						
						// we have reached the breakpoint, so swap parents; we want the "current strand" variables to change, so no std::swap()
						parent1_iter = parent2_iter;	parent1_iter_max = parent2_iter_max;	parent_genome_1 = parent_genome_2;
						parent2_iter = parent_iter;		parent2_iter_max = parent_iter_max;		parent_genome_2 = parent_genome;
						parent_iter = parent1_iter;		parent_iter_max = parent1_iter_max;		parent_genome = parent_genome_1;
						
						// skip over anything in the new parent that occurs prior to the breakpoint; it was not the active strand
						while (parent_iter != parent_iter_max && (mut_block_ptr + *parent_iter)->position_ < breakpoint)
							parent_iter++;
						
						// we have now handled the current breakpoint, so move on to the next breakpoint; advance the enclosing for loop here
						break_index++;
						
						// if we just handled the last breakpoint, which is guaranteed to be at or beyond lastPosition+1, then we are done
						if (break_index == break_index_max)
							break;
						
						// otherwise, figure out the new breakpoint, and continue looping on the current mutation run, which needs to be finished
						breakpoint = all_breakpoints[break_index];
						break_mutrun_index = breakpoint / mutrun_length;
						
						// if the next breakpoint is outside this mutation run, then finish the run and break out
						if (break_mutrun_index > this_mutrun_index)
						{
							while (parent_iter != parent_iter_max)
								child_mutrun->emplace_back(*(parent_iter++));
							
							break_index--;	// the outer loop will want to handle the current breakpoint again at the mutation-run level
							break;
						}
					}
					
					// We have completed this run
					++first_uncompleted_mutrun;
				}
				else
				{
					// The breakpoint occurs *between* runs, so just switch parent strands and the breakpoint is handled
					parent_genome_1 = parent_genome_2;
					parent_genome_2 = parent_genome;
					parent_genome = parent_genome_1;
				}
			}
		}
	}
	else
	{
		// we have at least one new mutation, so set up for that case (which splits into two cases below)
		
		// start with a clean slate in the child genome
		child_genome.clear_to_nullptr();
		
		int mutrun_length = child_genome.mutrun_length_;
		int mutrun_count = child_genome.mutrun_count_;
		
		// create vector with the mutations to be added
		MutationRun &mutations_to_add = *MutationRun::NewMutationRun();		// take from shared pool of used objects;
		
		for (int k = 0; k < num_mutations; k++)
		{
			MutationIndex new_mutation = p_chromosome.DrawNewMutation(p_parent_sex, p_source_subpop_id, p_generation);
			
			mutations_to_add.insert_sorted_mutation(new_mutation);	// keeps it sorted; since few mutations are expected, this is fast
			
			// no need to worry about pure_neutral_ or all_pure_neutral_DFE_ here; the mutation is drawn from a registered genomic element type
			// we can't handle the stacking policy here, since we don't yet know what the context of the new mutation will be; we do it below
			// we add the new mutation to the registry below, if the stacking policy says the mutation can actually be added
		}
		
		Mutation *mut_block_ptr = gSLiM_Mutation_Block;
		const MutationIndex *mutation_iter		= mutations_to_add.begin_pointer_const();
		const MutationIndex *mutation_iter_max	= mutations_to_add.end_pointer_const();
		
		MutationIndex mutation_iter_mutation_index;
		slim_position_t mutation_iter_pos;
		
		if (mutation_iter != mutation_iter_max) {
			mutation_iter_mutation_index = *mutation_iter;
			mutation_iter_pos = (mut_block_ptr + mutation_iter_mutation_index)->position_;
		} else {
			mutation_iter_mutation_index = -1;
			mutation_iter_pos = SLIM_INF_BASE_POSITION;
		}
		
		int mutation_mutrun_index = mutation_iter_pos / mutrun_length;
		
		Genome *parent_genome = parent_genome_1;
		int first_uncompleted_mutrun = 0;
		
		if (num_breakpoints == 0)
		{
			//
			// mutations without breakpoints; we have to be careful here not to touch the second strand, because it could be null
			//
			
			while (true)
			{
				// Copy over mutation runs until we arrive at the run in which the mutation occurs
				while (mutation_mutrun_index > first_uncompleted_mutrun)
				{
					child_genome.mutruns_[first_uncompleted_mutrun] = parent_genome->mutruns_[first_uncompleted_mutrun];
					++first_uncompleted_mutrun;
					
					if (first_uncompleted_mutrun >= mutrun_count)
						break;
				}
				
				if (first_uncompleted_mutrun >= mutrun_count)
					break;
				
				// The mutation occurs *inside* the run, so process the run by copying mutations
				int this_mutrun_index = first_uncompleted_mutrun;
				const MutationIndex *parent_iter		= parent_genome->mutruns_[this_mutrun_index]->begin_pointer_const();
				const MutationIndex *parent_iter_max	= parent_genome->mutruns_[this_mutrun_index]->end_pointer_const();
				MutationRun *child_mutrun = child_genome.WillCreateRun(this_mutrun_index);
				
				// add any additional new mutations that occur before the end of the mutation run; there is at least one
				do
				{
					// add any parental mutations that occur before or at the next new mutation's position
					while (parent_iter != parent_iter_max)
					{
						MutationIndex current_mutation = *parent_iter;
						slim_position_t current_mutation_pos = (mut_block_ptr + current_mutation)->position_;
						
						if (current_mutation_pos > mutation_iter_pos)
							break;
						
						child_mutrun->emplace_back(current_mutation);
						parent_iter++;
					}
					
					// add the new mutation, which might overlap with the last added old mutation
					if (child_mutrun->enforce_stack_policy_for_addition((mut_block_ptr + mutation_iter_mutation_index)->position_, (mut_block_ptr + mutation_iter_mutation_index)->mutation_type_ptr_))
					{
						// The mutation was passed by the stacking policy, so we can add it to the child genome and the registry
						child_mutrun->emplace_back(mutation_iter_mutation_index);
						mutation_registry_.emplace_back(mutation_iter_mutation_index);
					}
					else
					{
						// The mutation was rejected by the stacking policy, so we have to dispose of it
						// We no longer delete mutation objects; instead, we remove them from our shared pool
						(gSLiM_Mutation_Block + mutation_iter_mutation_index)->~Mutation();
						SLiM_DisposeMutationToBlock(mutation_iter_mutation_index);
					}
					
					if (++mutation_iter != mutation_iter_max) {
						mutation_iter_mutation_index = *mutation_iter;
						mutation_iter_pos = (mut_block_ptr + mutation_iter_mutation_index)->position_;
					} else {
						mutation_iter_mutation_index = -1;
						mutation_iter_pos = SLIM_INF_BASE_POSITION;
					}
					
					mutation_mutrun_index = mutation_iter_pos / mutrun_length;
				}
				while (mutation_mutrun_index == this_mutrun_index);
				
				// finish up any parental mutations that come after the last new mutation in the mutation run
				while (parent_iter != parent_iter_max)
					child_mutrun->emplace_back(*(parent_iter++));
				
				// We have completed this run
				++first_uncompleted_mutrun;
				
				if (first_uncompleted_mutrun >= mutrun_count)
					break;
			}
		}
		else
		{
			//
			// mutations and crossovers; this is the most complex case
			//
			
			// fix up the breakpoints vector; above we allow it to be completely empty, for maximal speed in the
			// 0-mutation/0-breakpoint case, but here we need a defined end breakpoint, so we add it now if necessary
			if (all_breakpoints.size() == 0)
				all_breakpoints.emplace_back(p_chromosome.last_position_mutrun_ + 1);
			
			int break_index_max = static_cast<int>(all_breakpoints.size());	// can be != num_breakpoints+1 due to gene conversion and dup removal!
			int break_index = 0;
			slim_position_t breakpoint = all_breakpoints[break_index];
			int break_mutrun_index = breakpoint / mutrun_length;
			
			while (true)	// loop over breakpoints until we have handled the last one, which comes at the end
			{
				if (mutation_mutrun_index < break_mutrun_index)
				{
					// Copy over mutation runs until we arrive at the run in which the mutation occurs
					while (mutation_mutrun_index > first_uncompleted_mutrun)
					{
						child_genome.mutruns_[first_uncompleted_mutrun] = parent_genome->mutruns_[first_uncompleted_mutrun];
						++first_uncompleted_mutrun;
						
						// We can't be done, since we have a mutation waiting to be placed, so we don't need to check
					}
					
					// Mutations can't occur between mutation runs the way breakpoints can, so we don't need to check that either
				}
				else
				{
					// Copy over mutation runs until we arrive at the run in which the breakpoint occurs
					while (break_mutrun_index > first_uncompleted_mutrun)
					{
						child_genome.mutruns_[first_uncompleted_mutrun] = parent_genome->mutruns_[first_uncompleted_mutrun];
						++first_uncompleted_mutrun;
						
						if (first_uncompleted_mutrun >= mutrun_count)
							break;
					}
					
					// Now we are supposed to process a breakpoint in first_uncompleted_mutrun; check whether that means we're done
					if (first_uncompleted_mutrun >= mutrun_count)
						break;
					
					// If the breakpoint occurs *between* runs, just switch parent strands and the breakpoint is handled
					if (breakpoint == break_mutrun_index * mutrun_length)
					{
						parent_genome_1 = parent_genome_2;
						parent_genome_2 = parent_genome;
						parent_genome = parent_genome_1;
						
						// go to next breakpoint; this advances the for loop
						if (++break_index == break_index_max)
							break;
						
						breakpoint = all_breakpoints[break_index];
						break_mutrun_index = breakpoint / mutrun_length;
						
						continue;
					}
				}
				
				// The event occurs *inside* the run, so process the run by copying mutations and switching strands
				int this_mutrun_index = first_uncompleted_mutrun;
				MutationRun *child_mutrun = child_genome.WillCreateRun(this_mutrun_index);
				const MutationIndex *parent1_iter		= parent_genome_1->mutruns_[this_mutrun_index]->begin_pointer_const();
				const MutationIndex *parent1_iter_max	= parent_genome_1->mutruns_[this_mutrun_index]->end_pointer_const();
				const MutationIndex *parent_iter		= parent1_iter;
				const MutationIndex *parent_iter_max	= parent1_iter_max;
				
				if (break_mutrun_index == this_mutrun_index)
				{
					const MutationIndex *parent2_iter		= parent_genome_2->mutruns_[this_mutrun_index]->begin_pointer_const();
					const MutationIndex *parent2_iter_max	= parent_genome_2->mutruns_[this_mutrun_index]->end_pointer_const();
					
					if (mutation_mutrun_index == this_mutrun_index)
					{
						//
						// =====  this_mutrun_index has both breakpoint(s) and new mutation(s); this is the really nasty case
						//
						
						while (true)
						{
							// while there are still old mutations in the parent before the current breakpoint...
							while (parent_iter != parent_iter_max)
							{
								MutationIndex current_mutation = *parent_iter;
								slim_position_t current_mutation_pos = (mut_block_ptr + current_mutation)->position_;
								
								if (current_mutation_pos >= breakpoint)
									break;
								
								// add any new mutations that occur before the parental mutation; we know the parental mutation is in this run, so these are too
								while (mutation_iter_pos < current_mutation_pos)
								{
									if (child_mutrun->enforce_stack_policy_for_addition((mut_block_ptr + mutation_iter_mutation_index)->position_, (mut_block_ptr + mutation_iter_mutation_index)->mutation_type_ptr_))
									{
										// The mutation was passed by the stacking policy, so we can add it to the child genome and the registry
										child_mutrun->emplace_back(mutation_iter_mutation_index);
										mutation_registry_.emplace_back(mutation_iter_mutation_index);
									}
									else
									{
										// The mutation was rejected by the stacking policy, so we have to dispose of it
										// We no longer delete mutation objects; instead, we remove them from our shared pool
										(gSLiM_Mutation_Block + mutation_iter_mutation_index)->~Mutation();
										SLiM_DisposeMutationToBlock(mutation_iter_mutation_index);
									}
									
									if (++mutation_iter != mutation_iter_max) {
										mutation_iter_mutation_index = *mutation_iter;
										mutation_iter_pos = (mut_block_ptr + mutation_iter_mutation_index)->position_;
									} else {
										mutation_iter_mutation_index = -1;
										mutation_iter_pos = SLIM_INF_BASE_POSITION;
									}
									
									mutation_mutrun_index = mutation_iter_pos / mutrun_length;
								}
								
								// add the old mutation; no need to check for a duplicate here since the parental genome is already duplicate-free
								child_mutrun->emplace_back(current_mutation);
								
								parent_iter++;
							}
							
							// add any new mutations that occur before the breakpoint; for these we have to check that they fall within this mutation run
							while ((mutation_iter_pos < breakpoint) && (mutation_mutrun_index == this_mutrun_index))
							{
								if (child_mutrun->enforce_stack_policy_for_addition((mut_block_ptr + mutation_iter_mutation_index)->position_, (mut_block_ptr + mutation_iter_mutation_index)->mutation_type_ptr_))
								{
									// The mutation was passed by the stacking policy, so we can add it to the child genome and the registry
									child_mutrun->emplace_back(mutation_iter_mutation_index);
									mutation_registry_.emplace_back(mutation_iter_mutation_index);
								}
								else
								{
									// The mutation was rejected by the stacking policy, so we have to dispose of it
									// We no longer delete mutation objects; instead, we remove them from our shared pool
									(gSLiM_Mutation_Block + mutation_iter_mutation_index)->~Mutation();
									SLiM_DisposeMutationToBlock(mutation_iter_mutation_index);
								}
								
								if (++mutation_iter != mutation_iter_max) {
									mutation_iter_mutation_index = *mutation_iter;
									mutation_iter_pos = (mut_block_ptr + mutation_iter_mutation_index)->position_;
								} else {
									mutation_iter_mutation_index = -1;
									mutation_iter_pos = SLIM_INF_BASE_POSITION;
								}
								
								mutation_mutrun_index = mutation_iter_pos / mutrun_length;
							}
							
							// we have finished the parental mutation run; if the breakpoint we are now working toward lies beyond the end of the
							// current mutation run, then we have completed this run and can exit to the outer loop which will handle the rest
							if (break_mutrun_index > this_mutrun_index)
								break;		// the outer loop will want to handle this breakpoint again at the mutation-run level
							
							// we have reached the breakpoint, so swap parents; we want the "current strand" variables to change, so no std::swap()
							parent1_iter = parent2_iter;	parent1_iter_max = parent2_iter_max;	parent_genome_1 = parent_genome_2;
							parent2_iter = parent_iter;		parent2_iter_max = parent_iter_max;		parent_genome_2 = parent_genome;
							parent_iter = parent1_iter;		parent_iter_max = parent1_iter_max;		parent_genome = parent_genome_1;
							
							// skip over anything in the new parent that occurs prior to the breakpoint; it was not the active strand
							while (parent_iter != parent_iter_max && (mut_block_ptr + *parent_iter)->position_ < breakpoint)
								parent_iter++;
							
							// we have now handled the current breakpoint, so move on; if we just handled the last breakpoint, then we are done
							if (++break_index == break_index_max)
								break;
							
							// otherwise, figure out the new breakpoint, and continue looping on the current mutation run, which needs to be finished
							breakpoint = all_breakpoints[break_index];
							break_mutrun_index = breakpoint / mutrun_length;
						}
						
						// if we just handled the last breakpoint, which is guaranteed to be at or beyond lastPosition+1, then we are done
						if (break_index == break_index_max)
							break;
						
						// We have completed this run
						++first_uncompleted_mutrun;
					}
					else
					{
						//
						// =====  this_mutrun_index has only breakpoint(s), no new mutations
						//
						
						while (true)
						{
							// while there are still old mutations in the parent before the current breakpoint...
							while (parent_iter != parent_iter_max)
							{
								MutationIndex current_mutation = *parent_iter;
								
								if ((mut_block_ptr + current_mutation)->position_ >= breakpoint)
									break;
								
								// add the old mutation; no need to check for a duplicate here since the parental genome is already duplicate-free
								child_mutrun->emplace_back(current_mutation);
								
								parent_iter++;
							}
							
							// we have reached the breakpoint, so swap parents; we want the "current strand" variables to change, so no std::swap()
							parent1_iter = parent2_iter;	parent1_iter_max = parent2_iter_max;	parent_genome_1 = parent_genome_2;
							parent2_iter = parent_iter;		parent2_iter_max = parent_iter_max;		parent_genome_2 = parent_genome;
							parent_iter = parent1_iter;		parent_iter_max = parent1_iter_max;		parent_genome = parent_genome_1;
							
							// skip over anything in the new parent that occurs prior to the breakpoint; it was not the active strand
							while (parent_iter != parent_iter_max && (mut_block_ptr + *parent_iter)->position_ < breakpoint)
								parent_iter++;
							
							// we have now handled the current breakpoint, so move on; if we just handled the last breakpoint, then we are done
							if (++break_index == break_index_max)
								break;
							
							// otherwise, figure out the new breakpoint, and continue looping on the current mutation run, which needs to be finished
							breakpoint = all_breakpoints[break_index];
							break_mutrun_index = breakpoint / mutrun_length;
							
							// if the next breakpoint is outside this mutation run, then finish the run and break out
							if (break_mutrun_index > this_mutrun_index)
							{
								while (parent_iter != parent_iter_max)
									child_mutrun->emplace_back(*(parent_iter++));
								
								break;	// the outer loop will want to handle this breakpoint again at the mutation-run level
							}
						}
						
						// if we just handled the last breakpoint, which is guaranteed to be at or beyond lastPosition+1, then we are done
						if (break_index == break_index_max)
							break;
						
						// We have completed this run
						++first_uncompleted_mutrun;
					}
				}
				else if (mutation_mutrun_index == this_mutrun_index)
				{
					//
					// =====  this_mutrun_index has only new mutation(s), no breakpoints
					//
					
					// add any additional new mutations that occur before the end of the mutation run; there is at least one
					do
					{
						// add any parental mutations that occur before or at the next new mutation's position
						while (parent_iter != parent_iter_max)
						{
							MutationIndex current_mutation = *parent_iter;
							slim_position_t current_mutation_pos = (mut_block_ptr + current_mutation)->position_;
							
							if (current_mutation_pos > mutation_iter_pos)
								break;
							
							child_mutrun->emplace_back(current_mutation);
							parent_iter++;
						}
						
						// add the new mutation, which might overlap with the last added old mutation
						if (child_mutrun->enforce_stack_policy_for_addition((mut_block_ptr + mutation_iter_mutation_index)->position_, (mut_block_ptr + mutation_iter_mutation_index)->mutation_type_ptr_))
						{
							// The mutation was passed by the stacking policy, so we can add it to the child genome and the registry
							child_mutrun->emplace_back(mutation_iter_mutation_index);
							mutation_registry_.emplace_back(mutation_iter_mutation_index);
						}
						else
						{
							// The mutation was rejected by the stacking policy, so we have to dispose of it
							// We no longer delete mutation objects; instead, we remove them from our shared pool
							(gSLiM_Mutation_Block + mutation_iter_mutation_index)->~Mutation();
							SLiM_DisposeMutationToBlock(mutation_iter_mutation_index);
						}
						
						if (++mutation_iter != mutation_iter_max) {
							mutation_iter_mutation_index = *mutation_iter;
							mutation_iter_pos = (mut_block_ptr + mutation_iter_mutation_index)->position_;
						} else {
							mutation_iter_mutation_index = -1;
							mutation_iter_pos = SLIM_INF_BASE_POSITION;
						}
						
						mutation_mutrun_index = mutation_iter_pos / mutrun_length;
					}
					while (mutation_mutrun_index == this_mutrun_index);
					
					// finish up any parental mutations that come after the last new mutation in the mutation run
					while (parent_iter != parent_iter_max)
						child_mutrun->emplace_back(*(parent_iter++));
					
					// We have completed this run
					++first_uncompleted_mutrun;
				}
				else
				{
					EIDOS_TERMINATION << "ERROR (Population::DoCrossoverMutation): (internal error) logic fail." << EidosTerminate();
				}
			}
		}
		
		MutationRun::FreeMutationRun(&mutations_to_add);
	}
	
	// debugging check
#if 0
	for (int i = 0; i < child_genome.mutrun_count_; ++i)
		if (child_genome.mutruns_[i].get() == nullptr)
			EIDOS_TERMINATION << "ERROR (Population::DoCrossoverMutation): (internal error) null mutation run left at end of crossover-mutation." << EidosTerminate();
#endif
}

void Population::DoClonalMutation(Subpopulation *p_subpop, Subpopulation *p_source_subpop, slim_popsize_t p_child_genome_index, slim_objectid_t p_source_subpop_id, slim_popsize_t p_parent_genome_index, const Chromosome &p_chromosome, slim_generation_t p_generation, IndividualSex p_child_sex)
{
#pragma unused(p_child_sex)
#ifdef DEBUG
	if (p_child_sex == IndividualSex::kUnspecified)
		EIDOS_TERMINATION << "ERROR (Population::DoClonalMutation): Child sex cannot be IndividualSex::kUnspecified." << EidosTerminate();
#endif
	
	Genome &child_genome = p_subpop->child_genomes_[p_child_genome_index];
	GenomeType child_genome_type = child_genome.Type();
	Genome *parent_genome = &(p_source_subpop->parent_genomes_[p_parent_genome_index]);
	GenomeType parent_genome_type = parent_genome->Type();
	
	if (child_genome_type != parent_genome_type)
		EIDOS_TERMINATION << "ERROR (Population::DoClonalMutation): Mismatch between parent and child genome types (type != type)." << EidosTerminate();
	
	// check for null cases
	bool child_genome_null = child_genome.IsNull();
	bool parent_genome_null = parent_genome->IsNull();
	
	if (child_genome_null != parent_genome_null)
		EIDOS_TERMINATION << "ERROR (Population::DoClonalMutation): Mismatch between parent and child genome types (null != null)." << EidosTerminate();
	
	if (child_genome_null)
	{
		// a null strand cannot mutate, so we are done
		return;
	}
	
	// determine how many mutations and breakpoints we have
	int num_mutations = p_chromosome.DrawMutationCount(p_child_sex);	// the parent sex is the same as the child sex
	
	// mutations are usually rare, so let's streamline the case where none occur
	if (num_mutations == 0)
	{
		// no mutations, so the child genome is just a copy of the parental genome
		child_genome.copy_from_genome(p_source_subpop->parent_genomes_[p_parent_genome_index]);
	}
	else
	{
		// start with a clean slate in the child genome
		child_genome.clear_to_nullptr();
		
		// create vector with the mutations to be added
		MutationRun &mutations_to_add = *MutationRun::NewMutationRun();		// take from shared pool of used objects;
		
		for (int k = 0; k < num_mutations; k++)
		{
			MutationIndex new_mutation = p_chromosome.DrawNewMutation(p_child_sex, p_source_subpop_id, p_generation);	// the parent sex is the same as the child sex
			
			mutations_to_add.insert_sorted_mutation(new_mutation);	// keeps it sorted; since few mutations are expected, this is fast
			
			// no need to worry about pure_neutral_ or all_pure_neutral_DFE_ here; the mutation is drawn from a registered genomic element type
			// we can't handle the stacking policy here, since we don't yet know what the context of the new mutation will be; we do it below
			// we add the new mutation to the registry below, if the stacking policy says the mutation can actually be added
		}
		
		// loop over mutation runs and either (1) copy the mutrun pointer from the parent, or (2) make a new mutrun by modifying that of the parent
		Mutation *mut_block_ptr = gSLiM_Mutation_Block;
		
		int mutrun_count = child_genome.mutrun_count_;
		int mutrun_length = child_genome.mutrun_length_;
		
		const MutationIndex *mutation_iter		= mutations_to_add.begin_pointer_const();
		const MutationIndex *mutation_iter_max	= mutations_to_add.end_pointer_const();
		MutationIndex mutation_iter_mutation_index = *mutation_iter;
		slim_position_t mutation_iter_pos = (mut_block_ptr + mutation_iter_mutation_index)->position_;
		int mutation_iter_mutrun_index = mutation_iter_pos / mutrun_length;
		
		for (int run_index = 0; run_index < mutrun_count; ++run_index)
		{
			if (mutation_iter_mutrun_index > run_index)
			{
				// no mutations in this run, so just copy the run pointer
				child_genome.mutruns_[run_index] = parent_genome->mutruns_[run_index];
			}
			else
			{
				// interleave the parental genome with the new mutations
				MutationRun *child_run = child_genome.WillCreateRun(run_index);
				MutationRun *parent_run = parent_genome->mutruns_[run_index].get();
				const MutationIndex *parent_iter		= parent_run->begin_pointer_const();
				const MutationIndex *parent_iter_max	= parent_run->end_pointer_const();
				
				// while there is at least one new mutation left to place in this run... (which we know is true when we first reach here)
				do
				{
					// while an old mutation in the parent is before or at the next new mutation...
					while ((parent_iter != parent_iter_max) && ((mut_block_ptr + *parent_iter)->position_ <= mutation_iter_pos))
					{
						// we know the mutation is not already present, since mutations on the parent strand are already uniqued,
						// and new mutations are, by definition, new and thus cannot match the existing mutations
						child_run->emplace_back(*parent_iter);
						parent_iter++;
					}
					
					// while a new mutation in this run is before the next old mutation in the parent... (which we know is true when we first reach here)
					slim_position_t parent_iter_pos = (parent_iter == parent_iter_max) ? (SLIM_INF_BASE_POSITION) : (mut_block_ptr + *parent_iter)->position_;
					
					do
					{
						// we know the mutation is not already present, since mutations on the parent strand are already uniqued,
						// and new mutations are, by definition, new and thus cannot match the existing mutations
						if (child_run->enforce_stack_policy_for_addition(mutation_iter_pos, (mut_block_ptr + mutation_iter_mutation_index)->mutation_type_ptr_))
						{
							// The mutation was passed by the stacking policy, so we can add it to the child genome and the registry
							child_run->emplace_back(mutation_iter_mutation_index);
							mutation_registry_.emplace_back(mutation_iter_mutation_index);
						}
						else
						{
							// The mutation was rejected by the stacking policy, so we have to dispose of it
							// We no longer delete mutation objects; instead, we remove them from our shared pool
							(gSLiM_Mutation_Block + mutation_iter_mutation_index)->~Mutation();
							SLiM_DisposeMutationToBlock(mutation_iter_mutation_index);
						}
						
						// move to the next mutation
						mutation_iter++;
						
						if (mutation_iter == mutation_iter_max)
						{
							mutation_iter_mutation_index = -1;
							mutation_iter_pos = SLIM_INF_BASE_POSITION;
						}
						else
						{
							mutation_iter_mutation_index = *mutation_iter;
							mutation_iter_pos = (mut_block_ptr + mutation_iter_mutation_index)->position_;
						}
						
						mutation_iter_mutrun_index = mutation_iter_pos / mutrun_length;
						
						// if we're out of new mutations for this run, transfer down to the simpler loop below
						if (mutation_iter_mutrun_index != run_index)
							goto noNewMutationsLeft;
					}
					while (mutation_iter_pos < parent_iter_pos);
					
					// at this point we know we have a new mutation to place in this run, but it falls after the next parental mutation, so we loop back
				}
				while (true);
				
				// complete the mutation run after all new mutations within this run have been placed
			noNewMutationsLeft:
				while (parent_iter != parent_iter_max)
				{
					child_run->emplace_back(*parent_iter);
					parent_iter++;
				}
			}
		}
		
		MutationRun::FreeMutationRun(&mutations_to_add);
	}
}

#ifdef SLIMGUI
void Population::RecordFitness(slim_generation_t p_history_index, slim_objectid_t p_subpop_id, double p_fitness_value)
{
	FitnessHistory *history_rec_ptr = nullptr;
	
	// Find the existing history record, if it exists
	auto history_iter = fitness_histories_.find(p_subpop_id);
	
	if (history_iter != fitness_histories_.end())
		history_rec_ptr = &(history_iter->second);
	
	// If not, create a new history record and add it to our vector
	if (!history_rec_ptr)
	{
		FitnessHistory history_record;
		
		history_record.history_ = nullptr;
		history_record.history_length_ = 0;
		
		auto emplace_rec = fitness_histories_.emplace(std::pair<slim_objectid_t,FitnessHistory>(p_subpop_id, std::move(history_record)));
		
		if (emplace_rec.second)
			history_rec_ptr = &(emplace_rec.first->second);
	}
	
	// Assuming we now have a record, resize it as needed and insert the new value
	if (history_rec_ptr)
	{
		double *history = history_rec_ptr->history_;
		slim_generation_t history_length = history_rec_ptr->history_length_;
		
		if (p_history_index >= history_length)
		{
			slim_generation_t oldHistoryLength = history_length;
			
			history_length = p_history_index + 1000;			// give some elbow room for expansion
			history = (double *)realloc(history, history_length * sizeof(double));
			
			for (slim_generation_t i = oldHistoryLength; i < history_length; ++i)
				history[i] = NAN;
			
			// Copy the new values back into the history record
			history_rec_ptr->history_ = history;
			history_rec_ptr->history_length_ = history_length;
		}
		
		history[p_history_index] = p_fitness_value;
	}
}

// This method is used to record population statistics that are kept per generation for SLiMgui
void Population::SurveyPopulation(void)
{
	// Calculate mean fitness for this generation; this integrates the subpop mean fitness values from UpdateFitness()
	double totalFitness = 0.0;
	slim_popsize_t individualCount = 0;
	slim_generation_t historyIndex = sim_.generation_ - 1;	// zero-base: the first generation we put something in is generation 1, and we put it at index 0
	
	for (std::pair<const slim_objectid_t,Subpopulation*> &subpop_pair : *this)
	{ 
		Subpopulation *subpop = subpop_pair.second;
		
		totalFitness += subpop->parental_total_fitness_;
		individualCount += subpop->parent_subpop_size_;
		
		RecordFitness(historyIndex, subpop_pair.first, subpop->parental_total_fitness_ / subpop->parent_subpop_size_);
	}
	
	RecordFitness(historyIndex, -1, totalFitness / individualCount);
}
#endif

#ifdef SLIMGUI
// This method is used to tally up histogram metrics that are kept per mutation type for SLiMgui
void Population::AddTallyForMutationTypeAndBinNumber(int p_mutation_type_index, int p_mutation_type_count, slim_generation_t p_bin_number, slim_generation_t **p_buffer, uint32_t *p_bufferBins)
{
	slim_generation_t *buffer = *p_buffer;
	uint32_t bufferBins = *p_bufferBins;
	
	// A negative bin number can occur if the user is using the origin generation of mutations for their own purposes, as a tag field.  To protect against
	// crashing, we therefore clamp the bin number into [0, 1000000].  The upper bound is somewhat arbitrary, but we don't really want to allocate a larger
	// buffer than that anyway, and having values that large get clamped is not the end of the world, since these tallies are just for graphing.
	if (p_bin_number < 0)
		p_bin_number = 0;
	if (p_bin_number > 1000000)
		p_bin_number = 1000000;
	
	if (p_bin_number >= (int64_t)bufferBins)
	{
		int oldEntryCount = bufferBins * p_mutation_type_count;
		
		bufferBins = static_cast<uint32_t>(ceil((p_bin_number + 1) / 128.0) * 128.0);			// give ourselves some headroom so we're not reallocating too often
		int newEntryCount = bufferBins * p_mutation_type_count;
		
		buffer = (slim_generation_t *)realloc(buffer, newEntryCount * sizeof(slim_generation_t));
		
		// Zero out the new entries; the compiler should be smart enough to optimize this...
		for (int i = oldEntryCount; i < newEntryCount; ++i)
			buffer[i] = 0;
		
		// Since we reallocated the buffer, we need to set it back through our pointer parameters
		*p_buffer = buffer;
		*p_bufferBins = bufferBins;
	}
	
	// Add a tally to the appropriate bin
	(buffer[p_mutation_type_index + p_bin_number * p_mutation_type_count])++;
}
#endif

void Population::ValidateMutationFitnessCaches(void)
{
	Mutation *mut_block_ptr = gSLiM_Mutation_Block;
	const MutationIndex *registry_iter = mutation_registry_.begin_pointer_const();
	const MutationIndex *registry_iter_end = mutation_registry_.end_pointer_const();
	
	while (registry_iter != registry_iter_end)
	{
		MutationIndex mut_index = (*registry_iter++);
		Mutation *mut = mut_block_ptr + mut_index;
		slim_selcoeff_t sel_coeff = mut->selection_coeff_;
		slim_selcoeff_t dom_coeff = mut->mutation_type_ptr_->dominance_coeff_;
		
		mut->cached_one_plus_sel_ = (slim_selcoeff_t)std::max(0.0, 1.0 + sel_coeff);
		mut->cached_one_plus_dom_sel_ = (slim_selcoeff_t)std::max(0.0, 1.0 + dom_coeff * sel_coeff);
	}
}

void Population::RecalculateFitness(slim_generation_t p_generation)
{
	// calculate the fitnesses of the parents and make lookup tables; the main thing we do here is manage the fitness() callbacks
	// as per the SLiM design spec, we get the list of callbacks once, and use that list throughout this stage, but we construct
	// subsets of it for each subpopulation, so that UpdateFitness() can just use the callback list as given to it
	std::vector<SLiMEidosBlock*> fitness_callbacks = sim_.ScriptBlocksMatching(p_generation, SLiMEidosBlockType::SLiMEidosFitnessCallback, -1, -1, -1);
	std::vector<SLiMEidosBlock*> global_fitness_callbacks = sim_.ScriptBlocksMatching(p_generation, SLiMEidosBlockType::SLiMEidosFitnessGlobalCallback, -2, -1, -1);
	bool no_active_callbacks = true;
	
	for (SLiMEidosBlock *callback : fitness_callbacks)
		if (callback->active_)
		{
			no_active_callbacks = false;
			break;
		}
	if (no_active_callbacks)
		for (SLiMEidosBlock *callback : global_fitness_callbacks)
			if (callback->active_)
			{
				no_active_callbacks = false;
				break;
			}
	
	// Figure out how we are going to handle MutationRun nonneutral mutation caches; see mutation_run.h.  We need to assess
	// the state of callbacks and decide which of the three "regimes" we are in, and then depending upon that and what
	// regime we were in in the previous generation, invalidate nonneutral caches or allow them to persist.
	const std::map<slim_objectid_t,MutationType*> &mut_types = sim_.MutationTypes();
	int32_t last_regime = sim_.last_nonneutral_regime_;
	int32_t current_regime;
	
	if (no_active_callbacks)
	{
		current_regime = 1;
	}
	else
	{
		// First, we want to save off the old values of our flags that govern nonneutral caching
		for (auto muttype_iter : mut_types)
		{
			MutationType *muttype = muttype_iter.second;
			
			muttype->previous_set_neutral_by_global_active_callback_ = muttype->set_neutral_by_global_active_callback_;
			muttype->previous_subject_to_fitness_callback_ = muttype->subject_to_fitness_callback_;
		}
		
		// Then we assess which muttypes are being made globally neutral by a constant-value fitness callback
		bool all_active_callbacks_are_global_neutral_effects = true;
		
		for (auto muttype_iter : mut_types)
			(muttype_iter.second)->set_neutral_by_global_active_callback_ = false;
		
		for (SLiMEidosBlock *fitness_callback : fitness_callbacks)
		{
			if (fitness_callback->active_)
			{
				if (fitness_callback->subpopulation_id_ == -1)
				{
					const EidosASTNode *compound_statement_node = fitness_callback->compound_statement_node_;
					
					if (compound_statement_node->cached_value_)
					{
						// The script is a constant expression such as "{ return 1.1; }"
						EidosValue *result = compound_statement_node->cached_value_.get();
						
						if ((result->Type() == EidosValueType::kValueFloat) || (result->Count() == 1))
						{
							if (result->FloatAtIndex(0, nullptr) == 1.0)
							{
								// the callback returns 1.0, so it makes the mutation types to which it applies become neutral
								slim_objectid_t mutation_type_id = fitness_callback->mutation_type_id_;
								
								if (mutation_type_id != -1)
								{
									auto found_muttype_pair = mut_types.find(mutation_type_id);
									
									if (found_muttype_pair != mut_types.end())
										found_muttype_pair->second->set_neutral_by_global_active_callback_ = true;
								}
								
								// This is a constant neutral effect, so avoid dropping through to the flag set below
								continue;
							}
						}
					}
				}
				
				// if we reach this point, we have an active callback that is not a
				// global constant neutral effect, so set our flag and break out
				all_active_callbacks_are_global_neutral_effects = false;
				break;
			}
		}
		
		if (all_active_callbacks_are_global_neutral_effects)
		{
			// The only active callbacks are global (i.e. not subpop-specific) constant-effect neutral callbacks,
			// so we will use the set_neutral_by_global_active_callback flag in the muttypes that we set up above.
			// When that flag is true, the mut is neutral; when it is false, consult the selection coefficient.
			current_regime = 2;
		}
		else
		{
			// We have at least one active callback that is not a global constant-effect callback, so all
			// bets are off; any mutation of a muttype influenced by a callback must be considered non-neutral,
			// as governed by the flag set up below
			current_regime = 3;
			
			for (auto muttype_iter : mut_types)
				(muttype_iter.second)->subject_to_fitness_callback_ = false;
			
			for (SLiMEidosBlock *fitness_callback : fitness_callbacks)
			{
				slim_objectid_t mutation_type_id = fitness_callback->mutation_type_id_;
				
				if (mutation_type_id != -1)
				{
					auto found_muttype_pair = mut_types.find(mutation_type_id);
					
					if (found_muttype_pair != mut_types.end())
						found_muttype_pair->second->subject_to_fitness_callback_ = true;
				}
			}
		}
	}
	
	// trigger a recache of nonneutral mutation lists for some regime transitions; see mutation_run.h
	if (last_regime == 0)
		sim_.nonneutral_change_counter_++;
	else if ((current_regime == 1) && ((last_regime == 2) || (last_regime == 3)))
		sim_.nonneutral_change_counter_++;
	else if (current_regime == 2)
	{
		if (last_regime != 2)
			sim_.nonneutral_change_counter_++;
		else
		{
			// If we are in regime 2 this generation and were last generation as well, then if the way that
			// fitness callbacks are influencing mutation types is the same this generation as it was last
			// generation, we can actually carry over our nonneutral buffers.
			bool callback_state_identical = true;
			
			for (auto muttype_iter : mut_types)
			{
				MutationType *muttype = muttype_iter.second;
				
				if (muttype->set_neutral_by_global_active_callback_ != muttype->previous_set_neutral_by_global_active_callback_)
					callback_state_identical = false;
			}
			
			if (!callback_state_identical)
				sim_.nonneutral_change_counter_++;
		}
	}
	else if (current_regime == 3)
	{
		if (last_regime != 3)
			sim_.nonneutral_change_counter_++;
		else
		{
			// If we are in regime 3 this generation and were last generation as well, then if the way that
			// fitness callbacks are influencing mutation types is the same this generation as it was last
			// generation, we can actually carry over our nonneutral buffers.
			bool callback_state_identical = true;
			
			for (auto muttype_iter : mut_types)
			{
				MutationType *muttype = muttype_iter.second;
				
				if (muttype->subject_to_fitness_callback_ != muttype->previous_subject_to_fitness_callback_)
					callback_state_identical = false;
			}
			
			if (!callback_state_identical)
				sim_.nonneutral_change_counter_++;
		}
	}
	
	// move forward to the regime we just chose; UpdateFitness() can consult this to get the current regime
	sim_.last_nonneutral_regime_ = current_regime;
	
	if (no_active_callbacks)
	{
		std::vector<SLiMEidosBlock*> no_fitness_callbacks;
		
		for (std::pair<const slim_objectid_t,Subpopulation*> &subpop_pair : *this)
			subpop_pair.second->UpdateFitness(no_fitness_callbacks, no_fitness_callbacks);
	}
	else
	{
		for (std::pair<const slim_objectid_t,Subpopulation*> &subpop_pair : *this)
		{
			slim_objectid_t subpop_id = subpop_pair.first;
			Subpopulation *subpop = subpop_pair.second;
			std::vector<SLiMEidosBlock*> subpop_fitness_callbacks;
			std::vector<SLiMEidosBlock*> subpop_global_fitness_callbacks;
			
			// Get fitness callbacks that apply to this subpopulation
			for (SLiMEidosBlock *callback : fitness_callbacks)
			{
				slim_objectid_t callback_subpop_id = callback->subpopulation_id_;
				
				if ((callback_subpop_id == -1) || (callback_subpop_id == subpop_id))
					subpop_fitness_callbacks.emplace_back(callback);
			}
			for (SLiMEidosBlock *callback : global_fitness_callbacks)
			{
				slim_objectid_t callback_subpop_id = callback->subpopulation_id_;
				
				if ((callback_subpop_id == -1) || (callback_subpop_id == subpop_id))
					subpop_global_fitness_callbacks.emplace_back(callback);
			}
			
			// Update fitness values, using the callbacks
			subpop->UpdateFitness(subpop_fitness_callbacks, subpop_global_fitness_callbacks);
		}
	}
}

// Clear all parental genomes to use nullptr for their mutation runs, so they don't mess up our MutationRun refcounts
void Population::ClearParentalGenomes(void)
{
	for (const std::pair<const slim_objectid_t,Subpopulation*> &subpop_pair : *this)
	{
		Subpopulation *subpop = subpop_pair.second;
		slim_popsize_t subpop_genome_count = 2 * subpop->parent_subpop_size_;
		std::vector<Genome> &subpop_genomes = subpop->parent_genomes_;
		
		for (slim_popsize_t i = 0; i < subpop_genome_count; i++)
			subpop_genomes[i].clear_to_nullptr();
	}
	
	// We have to clear out removed subpops, too, for as long as they stick around
	for (Subpopulation *subpop : removed_subpops_)
	{
		{
			slim_popsize_t subpop_genome_count = 2 * subpop->parent_subpop_size_;
			std::vector<Genome> &subpop_genomes = subpop->parent_genomes_;
			
			for (slim_popsize_t i = 0; i < subpop_genome_count; i++)
				subpop_genomes[i].clear_to_nullptr();
		}
		
		{
			slim_popsize_t subpop_genome_count = 2 * subpop->child_subpop_size_;
			std::vector<Genome> &subpop_genomes = subpop->child_genomes_;
			
			for (slim_popsize_t i = 0; i < subpop_genome_count; i++)
				subpop_genomes[i].clear_to_nullptr();
		}
	}
}

// Scan through all mutation runs in the simulation and unique them
void Population::UniqueMutationRuns(void)
{
#if SLIM_DEBUG_MUTATION_RUNS
	clock_t begin = clock();
#endif
	std::multimap<int64_t, MutationRun *> runmap;
	int64_t total_mutruns = 0, total_hash_collisions = 0, total_identical = 0, total_uniqued_away = 0, total_preexisting = 0, total_final = 0;
	
	int64_t operation_id = ++gSLiM_MutationRun_OperationID;
	
	for (const std::pair<const slim_objectid_t,Subpopulation*> &subpop_pair : *this)
	{
		Subpopulation *subpop = subpop_pair.second;
		slim_popsize_t subpop_genome_count = (child_generation_valid_ ? 2 * subpop->child_subpop_size_ : 2 * subpop->parent_subpop_size_);
		std::vector<Genome> &subpop_genomes = (child_generation_valid_ ? subpop->child_genomes_ : subpop->parent_genomes_);
		
		for (slim_popsize_t genome_index = 0; genome_index < subpop_genome_count; genome_index++)
		{
			Genome &genome = subpop_genomes[genome_index];
			int32_t mutrun_count = genome.mutrun_count_;
			
			for (int mutrun_index = 0; mutrun_index < mutrun_count; ++mutrun_index)
			{
				MutationRun *mut_run = genome.mutruns_[mutrun_index].get();
				
				if (mut_run)
				{
					bool first_sight_of_this_mutrun = false;
					
					total_mutruns++;
					
					if (mut_run->operation_id_ != operation_id)
					{
						// Mark each new run we encounter with the operation ID, to count the preexisting number of runs
						total_preexisting++;
						mut_run->operation_id_ = operation_id;
						first_sight_of_this_mutrun = true;
					}
					
					// Calculate a hash for this mutrun.  Note that we could store computed hashes into the runs above, so that
					// we only hash each pre-existing run once; but that would require an int64_t more storage per mutrun, and
					// the memory overhead doesn't presently seem worth the very slight performance gain it would usually provide
					int64_t hash = mut_run->Hash();
					
					// See if we have any mutruns already defined with this hash.  Note that we actually want to do this search
					// even when first_sight_of_this_mutrun = true, because we want to find hash collisions, which may be other
					// runs that are identical to us despite being separate objects.  That is, in fact, kind of the point.
					auto range = runmap.equal_range(hash);		// pair<Iter, Iter>
					
					if (range.first == range.second)
					{
						// No previous mutrun found with this hash, so add this mutrun to the multimap
						runmap.insert(std::pair<int64_t, MutationRun *>(hash, mut_run));
						total_final++;
					}
					else
					{
						// There is at least one hit; first cycle through the hits and see if any of them are pointer-identical
						for (auto hash_iter = range.first; hash_iter != range.second; ++hash_iter)
						{
							if (mut_run == hash_iter->second)
							{
								total_identical++;
								goto is_identical;
							}
						}
						
						// OK, we have no pointer-identical matches; check for a duplicate using Identical()
						for (auto hash_iter = range.first; hash_iter != range.second; ++hash_iter)
						{
							MutationRun *hash_run = hash_iter->second;
							
							if (mut_run->Identical(*hash_run))
							{
								genome.mutruns_[mutrun_index].reset(hash_run);
								total_identical++;
								
								// We will unique away all references to this mutrun, but we only want to count it once
								if (first_sight_of_this_mutrun)
									total_uniqued_away++;
								goto is_identical;
							}
						}
						
						// If there was no identical match, then we have a hash collision; put it in the multimap
						runmap.insert(std::pair<int64_t, MutationRun *>(hash, mut_run));
						total_hash_collisions++;
						total_final++;
						
					is_identical:
						;
					}
				}
			}
		}
	}
	
#if SLIM_DEBUG_MUTATION_RUNS
	clock_t end = clock();
	double time_spent = static_cast<double>(end - begin) / CLOCKS_PER_SEC;
	
	std::cout << "UniqueMutationRuns(): \n   " << total_mutruns << " run pointers analyzed\n   " << total_preexisting << " runs pre-existing\n   " << total_uniqued_away << " duplicate runs discovered and uniqued away\n   " << (total_mutruns - total_identical) << " final uniqued mutation runs\n   " << total_hash_collisions << " hash collisions\n   " << time_spent << " seconds elapsed" << std::endl;
#endif
	
	if (total_final != total_mutruns - total_identical)
		EIDOS_TERMINATION << "ERROR (Population::UniqueMutationRuns): (internal error) bookkeeping error in mutation run uniquing." << EidosTerminate();
}

#ifndef __clang_analyzer__
void Population::SplitMutationRuns(int32_t p_new_mutrun_count)
{
	// clear out all of the child genomes since they also need to be resized; might as well do it up front
	for (const std::pair<const slim_objectid_t,Subpopulation*> &subpop_pair : *this)
	{
		Subpopulation *subpop = subpop_pair.second;
		slim_popsize_t subpop_genome_count = 2 * subpop->child_subpop_size_;
		std::vector<Genome> &subpop_genomes = subpop->child_genomes_;
		
		// for every genome
		for (slim_popsize_t genome_index = 0; genome_index < subpop_genome_count; genome_index++)
		{
			Genome &genome = subpop_genomes[genome_index];
			
			if (!genome.IsNull())
			{
				int32_t old_mutrun_count = genome.mutrun_count_;
				int32_t old_mutrun_length = genome.mutrun_length_;
				int32_t new_mutrun_count = old_mutrun_count << 1;
				int32_t new_mutrun_length = old_mutrun_length >> 1;
				
				genome.clear_to_nullptr();
				if (genome.mutruns_ != genome.run_buffer_)
					delete[] genome.mutruns_;
				genome.mutruns_ = nullptr;
				
				genome.mutrun_count_ = new_mutrun_count;
				genome.mutrun_length_ = new_mutrun_length;
				
				if (new_mutrun_count <= SLIM_GENOME_MUTRUN_BUFSIZE)
					genome.mutruns_ = genome.run_buffer_;
				else
					genome.mutruns_ = new MutationRun_SP[new_mutrun_count];
				
				// Install empty MutationRun objects; I think this is not necessary, since this is the
				// child generation, which will not be accessed by anybody until crossover-mutation
				//for (int run_index = 0; run_index < new_mutrun_count; ++run_index)
				//	genome.mutruns_[run_index] = MutationRun_SP(MutationRun::NewMutationRun());
			}
		}
	}
	
	// make a map to keep track of which mutation runs split into which new runs
	std::unordered_map<MutationRun *, std::pair<MutationRun *, MutationRun *>> split_map;
	std::vector<MutationRun_SP> mutrun_retain;
	MutationRun **mutruns_buf = (MutationRun **)malloc(p_new_mutrun_count * sizeof(MutationRun *));
	int mutruns_buf_index;
	
	// for every subpop
	for (const std::pair<const slim_objectid_t,Subpopulation*> &subpop_pair : *this)
	{
		Subpopulation *subpop = subpop_pair.second;
		slim_popsize_t subpop_genome_count = 2 * subpop->parent_subpop_size_;
		std::vector<Genome> &subpop_genomes = subpop->parent_genomes_;
		
		// for every genome
		for (slim_popsize_t genome_index = 0; genome_index < subpop_genome_count; genome_index++)
		{
			Genome &genome = subpop_genomes[genome_index];
			
			if (!genome.IsNull())
			{
				int32_t old_mutrun_count = genome.mutrun_count_;
				int32_t old_mutrun_length = genome.mutrun_length_;
				int32_t new_mutrun_count = old_mutrun_count << 1;
				int32_t new_mutrun_length = old_mutrun_length >> 1;
				
				// for every mutation run, fill up mutrun_buf with entries
				mutruns_buf_index = 0;
				
				for (int run_index = 0; run_index < old_mutrun_count; ++run_index)
				{
					MutationRun_SP &mutrun_sp_ref = genome.mutruns_[run_index];
					MutationRun *mutrun = mutrun_sp_ref.get();
					
					if (mutrun->UseCount() == 1)
					{
						// this mutrun is only referenced once, so we can just replace it without using the map
						MutationRun *first_half, *second_half;
						
						mutrun->split_run(&first_half, &second_half, new_mutrun_length * (mutruns_buf_index + 1));
						
						mutruns_buf[mutruns_buf_index++] = first_half;
						mutruns_buf[mutruns_buf_index++] = second_half;
					}
					else
					{
						// this mutrun is referenced more than once, so we want to use our map
						auto found_entry = split_map.find(mutrun);
						
						if (found_entry != split_map.end())
						{
							// it was in the map already, so just use the values from the map
							std::pair<MutationRun *, MutationRun *> &map_value = found_entry->second;
							MutationRun *first_half = map_value.first;
							MutationRun *second_half = map_value.second;
							
							mutruns_buf[mutruns_buf_index++] = first_half;
							mutruns_buf[mutruns_buf_index++] = second_half;
						}
						else
						{
							// it was not in the map, so make the new runs, and insert them into the map
							MutationRun *first_half, *second_half;
							
							mutrun->split_run(&first_half, &second_half, new_mutrun_length * (mutruns_buf_index + 1));
							
							mutruns_buf[mutruns_buf_index++] = first_half;
							mutruns_buf[mutruns_buf_index++] = second_half;
							
							split_map.insert(std::pair<MutationRun *, std::pair<MutationRun *, MutationRun *>>(mutrun, std::pair<MutationRun *, MutationRun *>(first_half, second_half)));
							
							// this vector slaps a retain on all the mapped runs so they don't get released, deallocated, and
							// reused out from under us, which would happen otherwise when their last occurrence was replaced
							mutrun_retain.push_back(mutrun_sp_ref);
						}
					}
				}
				
				// now replace the runs in the genome with those in mutrun_buf
				genome.clear_to_nullptr();
				if (genome.mutruns_ != genome.run_buffer_)
					delete[] genome.mutruns_;
				genome.mutruns_ = nullptr;
				
				genome.mutrun_count_ = new_mutrun_count;
				genome.mutrun_length_ = new_mutrun_length;
				
				if (new_mutrun_count <= SLIM_GENOME_MUTRUN_BUFSIZE)
					genome.mutruns_ = genome.run_buffer_;
				else
					genome.mutruns_ = new MutationRun_SP[new_mutrun_count];
				
				for (int run_index = 0; run_index < new_mutrun_count; ++run_index)
					genome.mutruns_[run_index].reset(mutruns_buf[run_index]);
			}
		}
	}
	
	if (mutruns_buf)
		free(mutruns_buf);
}
#else
// the static analyzer has a lot of trouble understanding this method
void Population::SplitMutationRuns(int32_t p_new_mutrun_count)
{
}
#endif

// define a hash function for std::pair<MutationRun *, MutationRun *> so we can use it in std::unordered_map below
// see https://stackoverflow.com/questions/32685540/c-unordered-map-with-pair-as-key-not-compiling
struct slim_pair_hash {
	template <class T1, class T2>
	std::size_t operator () (const std::pair<T1,T2> &p) const {
		auto h1 = std::hash<T1>{}(p.first);
		auto h2 = std::hash<T2>{}(p.second);
		
		// This is not a great hash function, but for our purposes it should actually be fine.
		// We don't expect identical pairs <A, A>, and if we have a pair <A, B> we don't
		// expect to get the reversed pair <B, A>, so this should not produce too many collisions.
		// If we do get collisions we could switch to MutationRun::Hash() instead, but it is
		// much slower, so this is probably better.
		return h1 ^ h2;  
	}
};

#ifndef __clang_analyzer__
void Population::JoinMutationRuns(int32_t p_new_mutrun_count)
{
	// clear out all of the child genomes since they also need to be resized; might as well do it up front
	for (const std::pair<const slim_objectid_t,Subpopulation*> &subpop_pair : *this)
	{
		Subpopulation *subpop = subpop_pair.second;
		slim_popsize_t subpop_genome_count = 2 * subpop->child_subpop_size_;
		std::vector<Genome> &subpop_genomes = subpop->child_genomes_;
		
		// for every genome
		for (slim_popsize_t genome_index = 0; genome_index < subpop_genome_count; genome_index++)
		{
			Genome &genome = subpop_genomes[genome_index];
			
			if (!genome.IsNull())
			{
				int32_t old_mutrun_count = genome.mutrun_count_;
				int32_t old_mutrun_length = genome.mutrun_length_;
				int32_t new_mutrun_count = old_mutrun_count >> 1;
				int32_t new_mutrun_length = old_mutrun_length << 1;
				
				genome.clear_to_nullptr();
				if (genome.mutruns_ != genome.run_buffer_)
					delete[] genome.mutruns_;
				genome.mutruns_ = nullptr;
				
				genome.mutrun_count_ = new_mutrun_count;
				genome.mutrun_length_ = new_mutrun_length;
				
				if (new_mutrun_count <= SLIM_GENOME_MUTRUN_BUFSIZE)
					genome.mutruns_ = genome.run_buffer_;
				else
					genome.mutruns_ = new MutationRun_SP[new_mutrun_count];
				
				// Install empty MutationRun objects; I think this is not necessary, since this is the
				// child generation, which will not be accessed by anybody until crossover-mutation
				//for (int run_index = 0; run_index < new_mutrun_count; ++run_index)
				//	genome.mutruns_[run_index] = MutationRun_SP(MutationRun::NewMutationRun());
			}
		}
	}
	
	// make a map to keep track of which mutation runs join into which new runs
	std::unordered_map<std::pair<MutationRun *, MutationRun *>, MutationRun *, slim_pair_hash> join_map;
	std::vector<MutationRun_SP> mutrun_retain;
	MutationRun **mutruns_buf = (MutationRun **)malloc(p_new_mutrun_count * sizeof(MutationRun *));
	int mutruns_buf_index;
	
	// for every subpop
	for (const std::pair<const slim_objectid_t,Subpopulation*> &subpop_pair : *this)
	{
		Subpopulation *subpop = subpop_pair.second;
		slim_popsize_t subpop_genome_count = 2 * subpop->parent_subpop_size_;
		std::vector<Genome> &subpop_genomes = subpop->parent_genomes_;
		
		// for every genome
		for (slim_popsize_t genome_index = 0; genome_index < subpop_genome_count; genome_index++)
		{
			Genome &genome = subpop_genomes[genome_index];
			
			if (!genome.IsNull())
			{
				int32_t old_mutrun_count = genome.mutrun_count_;
				int32_t old_mutrun_length = genome.mutrun_length_;
				int32_t new_mutrun_count = old_mutrun_count >> 1;
				int32_t new_mutrun_length = old_mutrun_length << 1;
				
				// for every mutation run, fill up mutrun_buf with entries
				mutruns_buf_index = 0;
				
				for (int run_index = 0; run_index < old_mutrun_count; run_index += 2)
				{
					MutationRun_SP &mutrun1_sp_ref = genome.mutruns_[run_index];
					MutationRun_SP &mutrun2_sp_ref = genome.mutruns_[run_index + 1];
					MutationRun *mutrun1 = mutrun1_sp_ref.get();
					MutationRun *mutrun2 = mutrun2_sp_ref.get();
					
					if ((mutrun1->UseCount() == 1) || (mutrun2->UseCount() == 1))
					{
						// one of these mutruns is only referenced once, so we can just replace them without using the map
						MutationRun *joined_run = MutationRun::NewMutationRun();	// take from shared pool of used objects
						
						joined_run->copy_from_run(*mutrun1);
						joined_run->emplace_back_bulk(mutrun2->begin_pointer_const(), mutrun2->size());
						
						mutruns_buf[mutruns_buf_index++] = joined_run;
					}
					else
					{
						// this mutrun is referenced more than once, so we want to use our map
						auto found_entry = join_map.find(std::pair<MutationRun *, MutationRun *>(mutrun1, mutrun2));
						
						if (found_entry != join_map.end())
						{
							// it was in the map already, so just use the values from the map
							MutationRun *map_value = found_entry->second;
							
							mutruns_buf[mutruns_buf_index++] = map_value;
						}
						else
						{
							// it was not in the map, so make the new runs, and insert them into the map
							MutationRun *joined_run = MutationRun::NewMutationRun();	// take from shared pool of used objects
							
							joined_run->copy_from_run(*mutrun1);
							joined_run->emplace_back_bulk(mutrun2->begin_pointer_const(), mutrun2->size());
							
							mutruns_buf[mutruns_buf_index++] = joined_run;
							
							join_map.insert(std::pair<std::pair<MutationRun *, MutationRun *>, MutationRun *>(std::pair<MutationRun *, MutationRun *>(mutrun1, mutrun2), joined_run));
							
							// this vector slaps a retain on all the mapped runs so they don't get released, deallocated, and
							// reused out from under us, which would happen otherwise when their last occurrence was replaced
							mutrun_retain.push_back(mutrun1_sp_ref);
							mutrun_retain.push_back(mutrun2_sp_ref);
						}
					}
				}
				
				// now replace the runs in the genome with those in mutrun_buf
				genome.clear_to_nullptr();
				if (genome.mutruns_ != genome.run_buffer_)
					delete[] genome.mutruns_;
				genome.mutruns_ = nullptr;
				
				genome.mutrun_count_ = new_mutrun_count;
				genome.mutrun_length_ = new_mutrun_length;
				
				if (new_mutrun_count <= SLIM_GENOME_MUTRUN_BUFSIZE)
					genome.mutruns_ = genome.run_buffer_;
				else
					genome.mutruns_ = new MutationRun_SP[new_mutrun_count];
				
				for (int run_index = 0; run_index < new_mutrun_count; ++run_index)
					genome.mutruns_[run_index].reset(mutruns_buf[run_index]);
			}
		}
	}
	
	if (mutruns_buf)
		free(mutruns_buf);
}
#else
// the static analyzer has a lot of trouble understanding this method
void Population::JoinMutationRuns(int32_t p_new_mutrun_count)
{
}
#endif

// Tally mutations and remove fixed/lost mutations
void Population::MaintainRegistry(void)
{
	// go through all genomes and increment mutation reference counts; this updates total_genome_count_
	TallyMutationReferences(nullptr, true);
	
	// remove any mutations that have been eliminated or have fixed
	RemoveFixedMutations();
	
	// check that the mutation registry does not have any "zombies" – mutations that have been removed and should no longer be there
#if DEBUG_MUTATION_ZOMBIES
	CheckMutationRegistry();
#endif
	
	// debug output: assess mutation run usage patterns
#if SLIM_DEBUG_MUTATION_RUNS
	AssessMutationRuns();
#endif
}

// assess usage patterns of mutation runs across the simulation
void Population::AssessMutationRuns(void)
{
	slim_generation_t gen = sim_.Generation();
	
	if (gen % 1000 == 0)
	{
		// First, unique our runs; this is just for debugging the uniquing, and should be removed.  FIXME
		slim_refcount_t total_genome_count = 0, total_mutrun_count = 0, total_shared_mutrun_count = 0;
		int mutrun_count = 0, mutrun_length = 0, use_count_total = 0;
		int64_t mutation_total = 0;
		
		int64_t operation_id = ++gSLiM_MutationRun_OperationID;
		
		for (const std::pair<const slim_objectid_t,Subpopulation*> &subpop_pair : *this)
		{
			Subpopulation *subpop = subpop_pair.second;
			slim_popsize_t subpop_genome_count = 2 * subpop->child_subpop_size_;
			std::vector<Genome> &subpop_genomes = subpop->child_genomes_;
			
			for (slim_popsize_t i = 0; i < subpop_genome_count; i++)
			{
				Genome &genome = subpop_genomes[i];
				
				if (!genome.IsNull())
				{
					mutrun_count = genome.mutrun_count_;
					mutrun_length = genome.mutrun_length_;
					
					for (int run_index = 0; run_index < mutrun_count; ++run_index)
					{
						MutationRun *mutrun = genome.mutruns_[run_index].get();
						int mutrun_size = mutrun->size();
						
						total_mutrun_count++;
						mutation_total += mutrun_size;
						
						if (mutrun->operation_id_ != operation_id)
						{
							slim_refcount_t use_count = (slim_refcount_t)mutrun->UseCount();
							
							total_shared_mutrun_count++;
							use_count_total += use_count;
							
							mutrun->operation_id_ = operation_id;
						}
					}
					
					total_genome_count++;
				}
			}
		}
		
		std::cout << "***** Generation " << gen << ":" << std::endl;
		std::cout << "   Mutation count: " << mutation_registry_.size() << std::endl;
		std::cout << "   Genome count: " << total_genome_count << " (divided into " << mutrun_count << " mutation runs of length " << mutrun_length << ")" << std::endl;
		
		std::cout << "   Mutation run unshared: " << total_mutrun_count;
		if (total_mutrun_count) std::cout << " (containing " << (mutation_total / (double)total_mutrun_count) << " mutations on average)";
		std::cout << std::endl;
		
		std::cout << "   Mutation run actual: " << total_shared_mutrun_count;
		if (total_shared_mutrun_count) std::cout << " (mean use count " << (use_count_total / (double)total_shared_mutrun_count) << ")";
		std::cout << std::endl;
		
		std::cout << "*****" << std::endl;
	}
}

// step forward a generation: make the children become the parents
void Population::SwapGenerations(void)
{
	// dispose of any freed subpops
	if (removed_subpops_.size())
	{
		for (auto removed_subpop : removed_subpops_)
			delete removed_subpop;
		
		removed_subpops_.clear();
	}
	
	// make children the new parents; each subpop flips its child_generation_valid flag at the end of this call
	for (std::pair<const slim_objectid_t,Subpopulation*> &subpop_pair : *this)
		subpop_pair.second->SwapChildAndParentGenomes();
	
	// flip our flag to indicate that the good genomes are now in the parental generation, and the next child generation is ready to be produced
	child_generation_valid_ = false;
}

// count the total number of times that each Mutation in the registry is referenced by a population, and return the maximum possible number of references (i.e. fixation)
// the only tricky thing is that if we're running in the GUI, we also tally up references within the selected subpopulations only
slim_refcount_t Population::TallyMutationReferences(std::vector<Subpopulation*> *p_subpops_to_tally, bool p_force_recache)
{
	// First, figure out whether we're doing all subpops or a subset
	if (p_subpops_to_tally)
	{
		if (p_subpops_to_tally->size() == this->size())
		{
			// Rather than doing an equality test, we'll just assume that if there are N subpops and we've been asked to tally across N subpops,
			// we have been asked to tally across the whole population.  Hard to imagine a rational case that would violate that assumption.
			p_subpops_to_tally = nullptr;
		}
	}
	
	// Second, figure out whether the last tally was of the same thing, such that we can skip the work
	if (!p_force_recache && (cached_tally_genome_count_ != 0))
	{
		if ((p_subpops_to_tally == nullptr) && (last_tallied_subpops_.size() == 0))
		{
			return cached_tally_genome_count_;
		}
		else if (p_subpops_to_tally && last_tallied_subpops_.size() && (last_tallied_subpops_ == *p_subpops_to_tally))
		{
			return cached_tally_genome_count_;
		}
	}
	
	// Now do the actual tallying, since apparently it is necessary
	if (p_subpops_to_tally)
	{
		// When tallying just a subset of the subpops, we don't update the SLiMgui counts, nor do we update total_genome_count_
		
		// first zero out the refcounts in all registered Mutation objects
		SLiM_ZeroRefcountBlock(mutation_registry_);
		
		// then increment the refcounts through all pointers to Mutation in all genomes
		slim_refcount_t *refcount_block_ptr = gSLiM_Mutation_Refcounts;
		slim_refcount_t total_genome_count = 0;
		
		for (Subpopulation *subpop : *p_subpops_to_tally)
		{
			// Particularly for SLiMgui, we need to be able to tally mutation references after the generations have been swapped, i.e.
			// when the parental generation is active and the child generation is invalid.
			slim_popsize_t subpop_genome_count = (child_generation_valid_ ? 2 * subpop->child_subpop_size_ : 2 * subpop->parent_subpop_size_);
			std::vector<Genome> &subpop_genomes = (child_generation_valid_ ? subpop->child_genomes_ : subpop->parent_genomes_);
			
			for (slim_popsize_t i = 0; i < subpop_genome_count; i++)							// child genomes
			{
				Genome &genome = subpop_genomes[i];
				
				if (!genome.IsNull())
				{
					int mutrun_count = genome.mutrun_count_;
					
					for (int run_index = 0; run_index < mutrun_count; ++run_index)
					{
						MutationRun *mutrun = genome.mutruns_[run_index].get();
						const MutationIndex *genome_iter = mutrun->begin_pointer_const();
						const MutationIndex *genome_end_iter = mutrun->end_pointer_const();
						
						for (; genome_iter != genome_end_iter; ++genome_iter)
							++(*(refcount_block_ptr + *genome_iter));
					}
					
					total_genome_count++;	// count only non-null genomes to determine fixation
				}
			}
		}
		
		// set up the cache info
		last_tallied_subpops_ = *p_subpops_to_tally;
		cached_tally_genome_count_ = total_genome_count;
		
		return total_genome_count;
	}
	else
	{
		// We have a fast case, where we can tally using MutationRuns, and a slow case where we have to tally each
		// mutation in each Genome; our first order of business is to figure out which case we are using.
		bool can_tally_runs = true;
		
		// To tally using MutationRun, we should be at the point in the generation cycle where the registry is
		// maintained, so that other Genome objects have been cleared.  Otherwise, the tallies might not add up.
		if (!child_generation_valid_)
			can_tally_runs = false;
		
#ifdef SLIMGUI
		// If we're in SLiMgui, we need to figure out how we're going to handle its refcounts, which are
		// separate from slim's since the user can select just a subset of subpopulations.
		bool slimgui_subpop_subset_selected = false;
		
		for (const std::pair<const slim_objectid_t,Subpopulation*> &subpop_pair : *this)
			if (!subpop_pair.second->gui_selected_)
				slimgui_subpop_subset_selected = true;
		
		// If a subset of subpops is selected, we can't tally using MutationRun, because the MutationRun
		// refcounts are across all subpopulations.  (We could tally just for slim, but we would be left with
		// no way to get the sub-tallies for SLiMgui, so there's no point in doing that.)
		if (slimgui_subpop_subset_selected)
			can_tally_runs = false;
#endif
		
		// To tally using MutationRun, the refcounts of all active MutationRun objects should add up to the same
		// total as the total number of Genome objects being tallied across.  Otherwise, something is very wrong.
#ifdef DEBUG
		if (can_tally_runs)
		{
			slim_refcount_t total_genome_count = 0, tally_mutrun_ref_count = 0, total_mutrun_count = 0;
			int64_t operation_id = ++gSLiM_MutationRun_OperationID;
			
			for (const std::pair<const slim_objectid_t,Subpopulation*> &subpop_pair : *this)
			{
				Subpopulation *subpop = subpop_pair.second;
				
				slim_popsize_t subpop_genome_count = (child_generation_valid_ ? 2 * subpop->child_subpop_size_ : 2 * subpop->parent_subpop_size_);
				std::vector<Genome> &subpop_genomes = (child_generation_valid_ ? subpop->child_genomes_ : subpop->parent_genomes_);
				
				for (slim_popsize_t i = 0; i < subpop_genome_count; i++)							// child genomes
				{
					Genome &genome = subpop_genomes[i];
					
					if (!genome.IsNull())
					{
						subpop_genomes[i].TallyGenomeReferences(&tally_mutrun_ref_count, &total_mutrun_count, operation_id);
						total_genome_count++;
					}
				}
			}
			
			int mutrun_count = sim_.TheChromosome().mutrun_count_;
			
			if (total_genome_count * mutrun_count != tally_mutrun_ref_count)
				EIDOS_TERMINATION << "ERROR (Population::TallyMutationReferences): (internal error) tally != total genome count." << EidosTerminate();
			
			//std::cout << "Total genomes / MutationRuns: " << total_genome_count << " / " << total_mutrun_count << std::endl;
		}
#endif
		
		if (can_tally_runs)
		{
			//	FAST CASE: TALLY MUTATIONRUN OBJECTS AS CHUNKS
			//
			
			// Give the core work to our fast worker method; this is mostly for easier performance monitoring
			slim_refcount_t total_genome_count = TallyMutationReferences_FAST();
			
			// set up the cache info
			last_tallied_subpops_.clear();
			cached_tally_genome_count_ = total_genome_count;
			
			// set up the global genome counts
			total_genome_count_ = total_genome_count;
			
#ifdef SLIMGUI
			// SLiMgui can use this case if all subpops are selected; in that case, copy refcounts over to SLiMgui's info
			Mutation *mut_block_ptr = gSLiM_Mutation_Block;
			slim_refcount_t *refcount_block_ptr = gSLiM_Mutation_Refcounts;
			const MutationIndex *registry_iter = mutation_registry_.begin_pointer_const();
			const MutationIndex *registry_iter_end = mutation_registry_.end_pointer_const();
			
			while (registry_iter != registry_iter_end)
			{
				MutationIndex mut_index = *registry_iter;
				slim_refcount_t *refcount_ptr = refcount_block_ptr + mut_index;
				const Mutation *mutation = mut_block_ptr + mut_index;
				
				mutation->gui_reference_count_ = *refcount_ptr;
				registry_iter++;
			}
			
			gui_total_genome_count_ = total_genome_count;
#endif
			
			return total_genome_count;
		}
		else
		{
			//	SLOW CASE: TALLY EACH MUTATION INDIVIDUALLY
			//
			
			// When tallying the full population, we update SLiMgui counts as well, and we update total_genome_count_
			slim_refcount_t total_genome_count = 0;
#ifdef SLIMGUI
			slim_refcount_t gui_total_genome_count = 0;
			Mutation *mut_block_ptr = gSLiM_Mutation_Block;
#endif
			
			// first zero out the refcounts in all registered Mutation objects
#ifdef SLIMGUI
			// So, we have two different cases in SLiMgui that are both handled by the slow case here.  One is that all subpops are
			// selected in SLiMgui; in this case we can just copy the refcounts over after they have been tallied, so we don't
			// need to zero out the gui tallies here, or tally them separately below.  The other is that only some subpops are
			// selected in SLiMgui; in that case, we have to go the super-slow route and increment the gui tallies one by one,
			// so we have to zero them out here.
			if (slimgui_subpop_subset_selected)
			{
				const MutationIndex *registry_iter = mutation_registry_.begin_pointer_const();
				const MutationIndex *registry_iter_end = mutation_registry_.end_pointer_const();
				
				while (registry_iter != registry_iter_end)
				{
					MutationIndex mutation_index = *registry_iter;
					Mutation *mutation = mut_block_ptr + mutation_index;
					
					mutation->gui_reference_count_ = 0;
					registry_iter++;
				}
			}
#endif
			SLiM_ZeroRefcountBlock(mutation_registry_);
			
			// then increment the refcounts through all pointers to Mutation in all genomes
			slim_refcount_t *refcount_block_ptr = gSLiM_Mutation_Refcounts;
			
			for (const std::pair<const slim_objectid_t,Subpopulation*> &subpop_pair : *this)
			{
				Subpopulation *subpop = subpop_pair.second;
				
				// Particularly for SLiMgui, we need to be able to tally mutation references after the generations have been swapped, i.e.
				// when the parental generation is active and the child generation is invalid.
				slim_popsize_t subpop_genome_count = (child_generation_valid_ ? 2 * subpop->child_subpop_size_ : 2 * subpop->parent_subpop_size_);
				std::vector<Genome> &subpop_genomes = (child_generation_valid_ ? subpop->child_genomes_ : subpop->parent_genomes_);
				
#ifdef SLIMGUI
				// When running under SLiMgui, we need to tally up mutation references within the selected subpops, too; note
				// the else clause here drops outside of the #ifdef to the standard tally code.  Note that this clause is used
				// only when a subset of subpops in selected in SLiMgui; if all are selected, we can be smarter (below).
				if (slimgui_subpop_subset_selected && subpop->gui_selected_)
				{
					for (slim_popsize_t i = 0; i < subpop_genome_count; i++)							// child genomes
					{
						Genome &genome = subpop_genomes[i];
						
						if (!genome.IsNull())
						{
							int mutrun_count = genome.mutrun_count_;
							
							for (int run_index = 0; run_index < mutrun_count; ++run_index)
							{
								MutationRun *mutrun = genome.mutruns_[run_index].get();
								const MutationIndex *genome_iter = mutrun->begin_pointer_const();
								const MutationIndex *genome_end_iter = mutrun->end_pointer_const();
								
								while (genome_iter != genome_end_iter)
								{
									MutationIndex mut_index = *genome_iter;
									const Mutation *mutation = mut_block_ptr + mut_index;
									slim_refcount_t *refcount_ptr = refcount_block_ptr + mut_index;
									
									(*refcount_ptr)++;
									(mutation->gui_reference_count_)++;
									genome_iter++;
								}
							}
							
							total_genome_count++;	// count only non-null genomes to determine fixation
							gui_total_genome_count++;
						}
					}
				}
				else
#endif
				{
					for (slim_popsize_t i = 0; i < subpop_genome_count; i++)							// child genomes
					{
						Genome &genome = subpop_genomes[i];
						
						if (!genome.IsNull())
						{
							int mutrun_count = genome.mutrun_count_;
							
							for (int run_index = 0; run_index < mutrun_count; ++run_index)
							{
								MutationRun *mutrun = genome.mutruns_[run_index].get();
								const MutationIndex *genome_iter = mutrun->begin_pointer_const();
								const MutationIndex *genome_end_iter = mutrun->end_pointer_const();
								
								// Do 16 reps
								while (genome_iter + 16 <= genome_end_iter)
								{
									(*(refcount_block_ptr + (*genome_iter++)))++;
									(*(refcount_block_ptr + (*genome_iter++)))++;
									(*(refcount_block_ptr + (*genome_iter++)))++;
									(*(refcount_block_ptr + (*genome_iter++)))++;
									(*(refcount_block_ptr + (*genome_iter++)))++;
									(*(refcount_block_ptr + (*genome_iter++)))++;
									(*(refcount_block_ptr + (*genome_iter++)))++;
									(*(refcount_block_ptr + (*genome_iter++)))++;
									(*(refcount_block_ptr + (*genome_iter++)))++;
									(*(refcount_block_ptr + (*genome_iter++)))++;
									(*(refcount_block_ptr + (*genome_iter++)))++;
									(*(refcount_block_ptr + (*genome_iter++)))++;
									(*(refcount_block_ptr + (*genome_iter++)))++;
									(*(refcount_block_ptr + (*genome_iter++)))++;
									(*(refcount_block_ptr + (*genome_iter++)))++;
									(*(refcount_block_ptr + (*genome_iter++)))++;
								}
								
								// Finish off
								while (genome_iter != genome_end_iter)
									(*(refcount_block_ptr + (*genome_iter++)))++;
							}
							
							total_genome_count++;	// count only non-null genomes to determine fixation
						}
					}
				}
			}
			
			// set up the cache info
			last_tallied_subpops_.clear();
			cached_tally_genome_count_ = total_genome_count;
			
			// set up the global genome counts
			total_genome_count_ = total_genome_count;
			
#ifdef SLIMGUI
			// If all subpops are selected in SLiMgui, we now copy the refcounts over, as in the fast case
			if (!slimgui_subpop_subset_selected)
			{
				const MutationIndex *registry_iter = mutation_registry_.begin_pointer_const();
				const MutationIndex *registry_iter_end = mutation_registry_.end_pointer_const();
				
				while (registry_iter != registry_iter_end)
				{
					MutationIndex mut_index = *registry_iter;
					slim_refcount_t *refcount_ptr = refcount_block_ptr + mut_index;
					const Mutation *mutation = mut_block_ptr + mut_index;
					
					mutation->gui_reference_count_ = *refcount_ptr;
					registry_iter++;
				}
				
				gui_total_genome_count = total_genome_count;
			}
			
			gui_total_genome_count_ = gui_total_genome_count;
#endif
			
			return total_genome_count;
		}
	}
}

slim_refcount_t Population::TallyMutationReferences_FAST(void)
{
	// first zero out the refcounts in all registered Mutation objects
	SLiM_ZeroRefcountBlock(mutation_registry_);
	
	// then increment the refcounts through all pointers to Mutation in all genomes
	slim_refcount_t total_genome_count = 0;
	int64_t operation_id = ++gSLiM_MutationRun_OperationID;
	
	for (const std::pair<const slim_objectid_t,Subpopulation*> &subpop_pair : *this)
	{
		Subpopulation *subpop = subpop_pair.second;
		
		// Particularly for SLiMgui, we need to be able to tally mutation references after the generations have been swapped, i.e.
		// when the parental generation is active and the child generation is invalid.
		slim_popsize_t subpop_genome_count = (child_generation_valid_ ? 2 * subpop->child_subpop_size_ : 2 * subpop->parent_subpop_size_);
		std::vector<Genome> &subpop_genomes = (child_generation_valid_ ? subpop->child_genomes_ : subpop->parent_genomes_);
		
		for (slim_popsize_t i = 0; i < subpop_genome_count; i++)							// child genomes
		{
			Genome &genome = subpop_genomes[i];
			
			if (!genome.IsNull())
			{
				genome.TallyGenomeMutationReferences(operation_id);
				total_genome_count++;	// count only non-null genomes to determine fixation
			}
		}
	}
	
	return total_genome_count;
}

// handle negative fixation (remove from the registry) and positive fixation (convert to Substitution), using reference counts from TallyMutationReferences()
// TallyMutationReferences() must have cached tallies across the whole population before this is called, or it will malfunction!
void Population::RemoveFixedMutations(void)
{
	// WE use stack-local MutationRun objects so they get disposed of properly via RAII; non-optimal
	// from a performance perspective, since they will do reallocs to reach their needed size, but
	// since this method is only called once per generation it shouldn't matter.
	MutationRun removed_mutation_accumulator;
	MutationRun fixed_mutation_accumulator;
	
#ifdef SLIMGUI
	int mutation_type_count = static_cast<int>(sim_.mutation_types_.size());
#endif
	
	// remove Mutation objects that are no longer referenced, freeing them; avoid using an iterator since it would be invalidated
	slim_refcount_t *refcount_block_ptr = gSLiM_Mutation_Refcounts;
	Mutation *mut_block_ptr = gSLiM_Mutation_Block;
	int registry_length = mutation_registry_.size();
	
	for (int i = 0; i < registry_length; ++i)
	{
		MutationIndex mutation_index = mutation_registry_[i];
		Mutation *mutation = mut_block_ptr + mutation_index;
		slim_refcount_t reference_count = *(refcount_block_ptr + mutation_index);
		bool remove_mutation = false;
		
		if (reference_count == 0)
		{
#if DEBUG_MUTATIONS
			SLIM_ERRSTREAM << "Mutation unreferenced, will remove: " << mutation << endl;
#endif

#ifdef SLIMGUI
			// If we're running under SLiMgui, make a note of the lifetime of the mutation
			slim_generation_t loss_time = sim_.generation_ - mutation->generation_;
			int mutation_type_index = mutation->mutation_type_ptr_->mutation_type_index_;
			
			AddTallyForMutationTypeAndBinNumber(mutation_type_index, mutation_type_count, loss_time / 10, &mutation_loss_times_, &mutation_loss_gen_slots_);
#endif
			
			remove_mutation = true;
		}
		else if ((reference_count == total_genome_count_) && (mutation->mutation_type_ptr_->convert_to_substitution_))
		{
#if DEBUG_MUTATIONS
			SLIM_ERRSTREAM << "Mutation fixed, will substitute: " << mutation << endl;
#endif
			
#ifdef SLIMGUI
			// If we're running under SLiMgui, make a note of the fixation time of the mutation
			slim_generation_t fixation_time = sim_.generation_ - mutation->generation_;
			int mutation_type_index = mutation->mutation_type_ptr_->mutation_type_index_;
			
			AddTallyForMutationTypeAndBinNumber(mutation_type_index, mutation_type_count, fixation_time / 10, &mutation_fixation_times_, &mutation_fixation_gen_slots_);
#endif
			
			// set a special flag value for the reference count to allow it to be found rapidly
			*(refcount_block_ptr + mutation_index) = -1;
			
			// add the fixed mutation to a vector, to be converted to a Substitution object below
			fixed_mutation_accumulator.insert_sorted_mutation(mutation_index);
			
			remove_mutation = true;
		}
		
		if (remove_mutation)
		{
			// We have an unreferenced mutation object, so we want to remove it quickly
			if (i == registry_length - 1)
			{
				mutation_registry_.pop_back();
				
				--registry_length;
			}
			else
			{
				MutationIndex last_mutation = mutation_registry_[registry_length - 1];
				mutation_registry_[i] = last_mutation;
				mutation_registry_.pop_back();
				
				--registry_length;
				--i;	// revisit this index
			}
			
			// We can't delete the mutation yet, because we might need to make a Substitution object from it, so add it to a vector for deletion below
			removed_mutation_accumulator.emplace_back(mutation_index);
		}
	}
	
	// replace fixed mutations with Substitution objects
	if (fixed_mutation_accumulator.size() > 0)
	{
		//std::cout << "Removing " << fixed_mutation_accumulator.size() << " fixed mutations..." << std::endl;
		
		// We remove fixed mutations from each MutationRun just once; this is the operation ID we use for that
		int64_t operation_id = ++gSLiM_MutationRun_OperationID;
		
		for (std::pair<const slim_objectid_t,Subpopulation*> &subpop_pair : *this)		// subpopulations
		{
			std::vector<Genome> &subpop_genomes = subpop_pair.second->child_genomes_;
			slim_popsize_t subpop_genome_count = 2 * subpop_pair.second->child_subpop_size_;
			
			for (slim_popsize_t i = 0; i < subpop_genome_count; i++)	// child genomes
			{
				Genome *genome = &(subpop_genomes[i]);
				
				if (!genome->IsNull())
				{
					// Loop over the mutations to remove, and take advantage of our mutation runs by scanning
					// for removal only within the runs that contain a mutation to be removed.  If there is
					// more than one mutation to be removed within the same run, the second time around the
					// runs will no-op the scan using operatiod_id.  The whole rest of the genomes can be skipped.
					int mutrun_length = genome->mutrun_length_;
					
					for (int mut_index = 0; mut_index < fixed_mutation_accumulator.size(); mut_index++)
					{
						MutationIndex mut_to_remove = fixed_mutation_accumulator[mut_index];
						slim_position_t mut_position = (mut_block_ptr + mut_to_remove)->position_;
						int mutrun_index = mut_position / mutrun_length;
						
						// Note that total_genome_count_ is not needed by RemoveFixedMutations(); refcounts were set to -1 above.
						genome->RemoveFixedMutations(operation_id, mutrun_index);
					}
				}
			}
		}
		
		slim_generation_t generation = sim_.Generation();
		
		for (int i = 0; i < fixed_mutation_accumulator.size(); i++)
			substitutions_.emplace_back(new Substitution(*(mut_block_ptr + fixed_mutation_accumulator[i]), generation));
	}
	
	// now we can delete (or zombify) removed mutation objects
	if (removed_mutation_accumulator.size() > 0)
	{
		for (int i = 0; i < removed_mutation_accumulator.size(); i++)
		{
			MutationIndex mutation = removed_mutation_accumulator[i];
			
#if DEBUG_MUTATION_ZOMBIES
			(gSLiM_Mutation_Block + mutation)->mutation_type_ptr_ = nullptr;	// render lethal
			(gSLiM_Mutation_Block + mutation)->reference_count_ = -1;			// zombie
#else
			// We no longer delete mutation objects; instead, we remove them from our shared pool
			(gSLiM_Mutation_Block + mutation)->~Mutation();
			SLiM_DisposeMutationToBlock(mutation);
#endif
		}
	}
}

void Population::CheckMutationRegistry(void)
{
	slim_refcount_t *refcount_block_ptr = gSLiM_Mutation_Refcounts;
	const MutationIndex *registry_iter = mutation_registry_.begin_pointer_const();
	const MutationIndex *registry_iter_end = mutation_registry_.end_pointer_const();
	
	// first check that we don't have any zombies in our registry
	for (; registry_iter != registry_iter_end; ++registry_iter)
		if (*(refcount_block_ptr + *registry_iter) == -1)
			SLIM_ERRSTREAM << "Zombie found in registry with address " << (*registry_iter) << std::endl;
	
	// then check that we don't have any zombies in any genomes
	for (const std::pair<const slim_objectid_t,Subpopulation*> &subpop_pair : *this)		// subpopulations
	{
		Subpopulation *subpop = subpop_pair.second;
		slim_popsize_t subpop_genome_count = 2 * subpop->child_subpop_size_;
		std::vector<Genome> &subpop_genomes = subpop->child_genomes_;
		
		for (slim_popsize_t i = 0; i < subpop_genome_count; i++)							// child genomes
		{
			Genome &genome = subpop_genomes[i];
			int mutrun_count = genome.mutrun_count_;
			
			for (int run_index = 0; run_index < mutrun_count; ++run_index)
			{
				MutationRun *mutrun = genome.mutruns_[run_index].get();
				const MutationIndex *genome_iter = mutrun->begin_pointer_const();
				const MutationIndex *genome_end_iter = mutrun->end_pointer_const();
				
				for (; genome_iter != genome_end_iter; ++genome_iter)
					if (*(refcount_block_ptr + *genome_iter) == -1)
						SLIM_ERRSTREAM << "Zombie found in genome with address " << (*genome_iter) << std::endl;
			}
		}
	}
}

// print all mutations and all genomes to a stream
void Population::PrintAll(std::ostream &p_out, bool p_output_spatial_positions) const
{
	// This method is written to be able to print the population whether child_generation_valid is true or false.
	// This is a little tricky, so be careful when modifying this code!
	
#if DO_MEMORY_CHECKS
	// This method can burn a huge amount of memory and get us killed, if we have a maximum memory usage.  It's nice to
	// try to check for that and terminate with a proper error message, to help the user diagnose the problem.
	int mem_check_counter = 0, mem_check_mod = 100;
	
	if (eidos_do_memory_checks)
		Eidos_CheckRSSAgainstMax("Population::PrintAll", "(The memory usage was already out of bounds on entry.)");
#endif
	
	// Figure out spatial position output.  If it was not requested, then we don't do it, and that's fine.  If it
	// was requested, then we output the number of spatial dimensions we're configured for (which might be zero).
	int spatial_output_count = (p_output_spatial_positions ? sim_.SpatialDimensionality() : 0);
	
	// Starting in SLiM 2.3, we output a version indicator at the top of the file so we can decode different versions, etc.
	// We use the same version numbers used in PrintAllBinary(), for simplicity.
	p_out << "Version: 3" << std::endl;
	
	// Output populations first
	p_out << "Populations:" << std::endl;
	for (const std::pair<const slim_objectid_t,Subpopulation*> &subpop_pair : *this)
	{
		Subpopulation *subpop = subpop_pair.second;
		slim_popsize_t subpop_size = (child_generation_valid_ ? subpop->child_subpop_size_ : subpop->parent_subpop_size_);
		double subpop_sex_ratio = (child_generation_valid_ ? subpop->child_sex_ratio_ : subpop->parent_sex_ratio_);
		
		p_out << "p" << subpop_pair.first << " " << subpop_size;
		
		// SEX ONLY
		if (subpop->sex_enabled_)
			p_out << " S " << subpop_sex_ratio;
		else
			p_out << " H";
		
		p_out << std::endl;
		
#if DO_MEMORY_CHECKS
		if (eidos_do_memory_checks)
		{
			mem_check_counter++;
			
			if (mem_check_counter % mem_check_mod == 0)
				Eidos_CheckRSSAgainstMax("Population::PrintAll", "(Out of memory while outputting population list.)");
		}
#endif
	}
	
	PolymorphismMap polymorphisms;
	Mutation *mut_block_ptr = gSLiM_Mutation_Block;
	
	// add all polymorphisms
	for (const std::pair<const slim_objectid_t,Subpopulation*> &subpop_pair : *this)			// go through all subpopulations
	{
		Subpopulation *subpop = subpop_pair.second;
		slim_popsize_t subpop_size = (child_generation_valid_ ? subpop->child_subpop_size_ : subpop->parent_subpop_size_);
		
		for (slim_popsize_t i = 0; i < 2 * subpop_size; i++)				// go through all children
		{
			Genome &genome = child_generation_valid_ ? subpop->child_genomes_[i] : subpop->parent_genomes_[i];
			int mutrun_count = genome.mutrun_count_;
			
			for (int run_index = 0; run_index < mutrun_count; ++run_index)
			{
				MutationRun *mutrun = genome.mutruns_[run_index].get();
				int mut_count = mutrun->size();
				const MutationIndex *mut_ptr = mutrun->begin_pointer_const();
				
				for (int mut_index = 0; mut_index < mut_count; ++mut_index)
					AddMutationToPolymorphismMap(&polymorphisms, mut_block_ptr + mut_ptr[mut_index]);
			}
			
#if DO_MEMORY_CHECKS
			if (eidos_do_memory_checks)
			{
				mem_check_counter++;
				
				if (mem_check_counter % mem_check_mod == 0)
					Eidos_CheckRSSAgainstMax("Population::PrintAll", "(Out of memory while assembling polymorphisms.)");
			}
#endif
		}
	}
	
	// print all polymorphisms
	p_out << "Mutations:"  << std::endl;
	
	for (const PolymorphismPair &polymorphism_pair : polymorphisms)
	{
		polymorphism_pair.second.Print(p_out);							// NOTE this added mutation_id_, BCH 11 June 2016
		
#if DO_MEMORY_CHECKS
		if (eidos_do_memory_checks)
		{
			mem_check_counter++;
			
			if (mem_check_counter % mem_check_mod == 0)
				Eidos_CheckRSSAgainstMax("Population::PrintAll", "(Out of memory while printing polymorphisms.)");
		}
#endif
	}
	
	// print all individuals
	p_out << "Individuals:" << std::endl;
	
	for (const std::pair<const slim_objectid_t,Subpopulation*> &subpop_pair : *this)			// go through all subpopulations
	{
		Subpopulation *subpop = subpop_pair.second;
		slim_objectid_t subpop_id = subpop_pair.first;
		slim_popsize_t subpop_size = (child_generation_valid_ ? subpop->child_subpop_size_ : subpop->parent_subpop_size_);
		slim_popsize_t first_male_index = (child_generation_valid_ ? subpop->child_first_male_index_ : subpop->parent_first_male_index_);
		
		for (slim_popsize_t i = 0; i < subpop_size; i++)				// go through all children
		{
			p_out << "p" << subpop_id << ":i" << i;						// individual identifier
			
			if (subpop->sex_enabled_)
				p_out << ((i < first_male_index) ? " F " : " M ");		// sex: SEX ONLY
			else
				p_out << " H ";											// hermaphrodite
			
			p_out << "p" << subpop_id << ":" << (i * 2);				// genome identifier 1
			p_out << " p" << subpop_id << ":" << (i * 2 + 1);			// genome identifier 2
			
			// output spatial position if requested
			if (spatial_output_count)
			{
				Individual &individual = (child_generation_valid_ ? subpop->child_individuals_[i] : subpop->parent_individuals_[i]);
				
				if (spatial_output_count >= 1)
					p_out << " " << individual.spatial_x_;
				if (spatial_output_count >= 2)
					p_out << " " << individual.spatial_y_;
				if (spatial_output_count >= 3)
					p_out << " " << individual.spatial_z_;
			}
			
			p_out << std::endl;
			
#if DO_MEMORY_CHECKS
			if (eidos_do_memory_checks)
			{
				mem_check_counter++;
				
				if (mem_check_counter % mem_check_mod == 0)
					Eidos_CheckRSSAgainstMax("Population::PrintAll", "(Out of memory while printing individuals.)");
			}
#endif
		}
	}
	
	// print all genomes
	p_out << "Genomes:" << std::endl;
	
	for (const std::pair<const slim_objectid_t,Subpopulation*> &subpop_pair : *this)			// go through all subpopulations
	{
		Subpopulation *subpop = subpop_pair.second;
		slim_objectid_t subpop_id = subpop_pair.first;
		slim_popsize_t subpop_size = (child_generation_valid_ ? subpop->child_subpop_size_ : subpop->parent_subpop_size_);
		
		for (slim_popsize_t i = 0; i < 2 * subpop_size; i++)							// go through all children
		{
			Genome &genome = child_generation_valid_ ? subpop->child_genomes_[i] : subpop->parent_genomes_[i];
			
			p_out << "p" << subpop_id << ":" << i << " " << genome.Type();
			
			if (genome.IsNull())
			{
				p_out << " <null>";
			}
			else
			{
				int mutrun_count = genome.mutrun_count_;
				
				for (int run_index = 0; run_index < mutrun_count; ++run_index)
				{
					MutationRun *mutrun = genome.mutruns_[run_index].get();
					int mut_count = mutrun->size();
					const MutationIndex *mut_ptr = mutrun->begin_pointer_const();
					
					for (int mut_index = 0; mut_index < mut_count; ++mut_index)
					{
						slim_polymorphismid_t polymorphism_id = FindMutationInPolymorphismMap(polymorphisms, mut_block_ptr + mut_ptr[mut_index]);
						
						if (polymorphism_id == -1)
							EIDOS_TERMINATION << "ERROR (Population::PrintAll): (internal error) polymorphism not found." << EidosTerminate();
						
						p_out << " " << polymorphism_id;
					}
				}
			}
			
			p_out << std::endl;
			
#if DO_MEMORY_CHECKS
			if (eidos_do_memory_checks)
			{
				mem_check_counter++;
				
				if (mem_check_counter % mem_check_mod == 0)
					Eidos_CheckRSSAgainstMax("Population::PrintAll", "(Out of memory while printing genomes.)");
			}
#endif
		}
	}
}

// print all mutations and all genomes to a stream in binary, for maximum reading speed
void Population::PrintAllBinary(std::ostream &p_out, bool p_output_spatial_positions) const
{
	// This function is written to be able to print the population whether child_generation_valid is true or false.
	// This is a little tricky, so be careful when modifying this code!
	
	// Figure out spatial position output.  If it was not requested, then we don't do it, and that's fine.  If it
	// was requested, then we output the number of spatial dimensions we're configured for (which might be zero).
	int32_t spatial_output_count = (int32_t)(p_output_spatial_positions ? sim_.SpatialDimensionality() : 0);
	
	int32_t section_end_tag = 0xFFFF0000;
	
	// Header section
	{
		// Write a 32-bit endianness tag
		int32_t endianness_tag = 0x12345678;
		
		p_out.write(reinterpret_cast<char *>(&endianness_tag), sizeof endianness_tag);
		
		// Write a format version tag
		int32_t version_tag = 3;																					// version 2 started with SLiM 2.1
																													// version 3 started with SLiM 2.3
		
		p_out.write(reinterpret_cast<char *>(&version_tag), sizeof version_tag);
		
		// Write the size of a double
		int32_t double_size = sizeof(double);
		
		p_out.write(reinterpret_cast<char *>(&double_size), sizeof double_size);
		
		// Write a test double, to ensure the same format is used on the reading machine
		double double_test = 1234567890.0987654321;
		
		p_out.write(reinterpret_cast<char *>(&double_test), sizeof double_test);
		
		// Write the sizes of the various SLiM types
		int32_t slim_generation_t_size = sizeof(slim_generation_t);
		int32_t slim_position_t_size = sizeof(slim_position_t);
		int32_t slim_objectid_t_size = sizeof(slim_objectid_t);
		int32_t slim_popsize_t_size = sizeof(slim_popsize_t);
		int32_t slim_refcount_t_size = sizeof(slim_refcount_t);
		int32_t slim_selcoeff_t_size = sizeof(slim_selcoeff_t);
		int32_t slim_mutationid_t_size = sizeof(slim_mutationid_t);													// Added in version 2
		int32_t slim_polymorphismid_t_size = sizeof(slim_polymorphismid_t);											// Added in version 2
		
		p_out.write(reinterpret_cast<char *>(&slim_generation_t_size), sizeof slim_generation_t_size);
		p_out.write(reinterpret_cast<char *>(&slim_position_t_size), sizeof slim_position_t_size);
		p_out.write(reinterpret_cast<char *>(&slim_objectid_t_size), sizeof slim_objectid_t_size);
		p_out.write(reinterpret_cast<char *>(&slim_popsize_t_size), sizeof slim_popsize_t_size);
		p_out.write(reinterpret_cast<char *>(&slim_refcount_t_size), sizeof slim_refcount_t_size);
		p_out.write(reinterpret_cast<char *>(&slim_selcoeff_t_size), sizeof slim_selcoeff_t_size);
		p_out.write(reinterpret_cast<char *>(&slim_mutationid_t_size), sizeof slim_mutationid_t_size);				// Added in version 2
		p_out.write(reinterpret_cast<char *>(&slim_polymorphismid_t_size), sizeof slim_polymorphismid_t_size);		// Added in version 2
		
		// Write the generation
		slim_generation_t generation = sim_.Generation();
		
		p_out.write(reinterpret_cast<char *>(&generation), sizeof generation);
		
		// Write the number of spatial coordinates we will write per individual.  Added in version 3.
		p_out.write(reinterpret_cast<char *>(&spatial_output_count), sizeof spatial_output_count);
	}
	
	// Write a tag indicating the section has ended
	p_out.write(reinterpret_cast<char *>(&section_end_tag), sizeof section_end_tag);
	
	// Populations section
	for (const std::pair<const slim_objectid_t,Subpopulation*> &subpop_pair : *this)
	{
		Subpopulation *subpop = subpop_pair.second;
		slim_objectid_t subpop_id = subpop_pair.first;
		slim_popsize_t subpop_size = (child_generation_valid_ ? subpop->child_subpop_size_ : subpop->parent_subpop_size_);
		double subpop_sex_ratio = (child_generation_valid_ ? subpop->child_sex_ratio_ : subpop->parent_sex_ratio_);
		
		// Write a tag indicating we are starting a new subpopulation
		int32_t subpop_start_tag = 0xFFFF0001;
		
		p_out.write(reinterpret_cast<char *>(&subpop_start_tag), sizeof subpop_start_tag);
		
		// Write the subpop identifier
		p_out.write(reinterpret_cast<char *>(&subpop_id), sizeof subpop_id);
		
		// Write the subpop size
		p_out.write(reinterpret_cast<char *>(&subpop_size), sizeof subpop_size);
		
		// Write a flag indicating whether this population has sexual or hermaphroditic
		int32_t sex_flag = (subpop->sex_enabled_ ? 1 : 0);
		
		p_out.write(reinterpret_cast<char *>(&sex_flag), sizeof sex_flag);
		
		// Write the sex ratio; if we are not sexual, this will be garbage, but that is fine, we want a constant-length record
		p_out.write(reinterpret_cast<char *>(&subpop_sex_ratio), sizeof subpop_sex_ratio);
		
		// now will come either a subpopulation start tag, or a section end tag
	}
	
	// Write a tag indicating the section has ended
	p_out.write(reinterpret_cast<char *>(&section_end_tag), sizeof section_end_tag);
	
	// Find all polymorphisms
	PolymorphismMap polymorphisms;
	Mutation *mut_block_ptr = gSLiM_Mutation_Block;
	
	for (const std::pair<const slim_objectid_t,Subpopulation*> &subpop_pair : *this)			// go through all subpopulations
	{
		Subpopulation *subpop = subpop_pair.second;
		slim_popsize_t subpop_size = (child_generation_valid_ ? subpop->child_subpop_size_ : subpop->parent_subpop_size_);
		
		for (slim_popsize_t i = 0; i < 2 * subpop_size; i++)				// go through all children
		{
			Genome &genome = child_generation_valid_ ? subpop->child_genomes_[i] : subpop->parent_genomes_[i];
			int mutrun_count = genome.mutrun_count_;
			
			for (int run_index = 0; run_index < mutrun_count; ++run_index)
			{
				MutationRun *mutrun = genome.mutruns_[run_index].get();
				int mut_count = mutrun->size();
				const MutationIndex *mut_ptr = mutrun->begin_pointer_const();
				
				for (int mut_index = 0; mut_index < mut_count; ++mut_index)
					AddMutationToPolymorphismMap(&polymorphisms, mut_block_ptr + mut_ptr[mut_index]);
			}
		}
	}
	
	// Write out the size of the mutation map, so we can allocate a vector rather than utilizing std::map when reading
	int32_t mutation_map_size = (int32_t)polymorphisms.size();
	
	p_out.write(reinterpret_cast<char *>(&mutation_map_size), sizeof mutation_map_size);
	
	// Mutations section
	for (const PolymorphismPair &polymorphism_pair : polymorphisms)
	{
		const Polymorphism &polymorphism = polymorphism_pair.second;
		const Mutation *mutation_ptr = polymorphism.mutation_ptr_;
		const MutationType *mutation_type_ptr = mutation_ptr->mutation_type_ptr_;
		
		slim_polymorphismid_t polymorphism_id = polymorphism.polymorphism_id_;
		int64_t mutation_id = mutation_ptr->mutation_id_;													// Added in version 2
		slim_objectid_t mutation_type_id = mutation_type_ptr->mutation_type_id_;
		slim_position_t position = mutation_ptr->position_;
		slim_selcoeff_t selection_coeff = mutation_ptr->selection_coeff_;
		slim_selcoeff_t dominance_coeff = mutation_type_ptr->dominance_coeff_;
		slim_objectid_t subpop_index = mutation_ptr->subpop_index_;
		slim_generation_t generation = mutation_ptr->generation_;
		slim_refcount_t prevalence = polymorphism.prevalence_;
		
		// Write a tag indicating we are starting a new mutation
		int32_t mutation_start_tag = 0xFFFF0002;
		
		p_out.write(reinterpret_cast<char *>(&mutation_start_tag), sizeof mutation_start_tag);
		
		// Write the mutation data
		p_out.write(reinterpret_cast<char *>(&polymorphism_id), sizeof polymorphism_id);
		p_out.write(reinterpret_cast<char *>(&mutation_id), sizeof mutation_id);							// Added in version 2
		p_out.write(reinterpret_cast<char *>(&mutation_type_id), sizeof mutation_type_id);
		p_out.write(reinterpret_cast<char *>(&position), sizeof position);
		p_out.write(reinterpret_cast<char *>(&selection_coeff), sizeof selection_coeff);
		p_out.write(reinterpret_cast<char *>(&dominance_coeff), sizeof dominance_coeff);
		p_out.write(reinterpret_cast<char *>(&subpop_index), sizeof subpop_index);
		p_out.write(reinterpret_cast<char *>(&generation), sizeof generation);
		p_out.write(reinterpret_cast<char *>(&prevalence), sizeof prevalence);
		
		// now will come either a mutation start tag, or a section end tag
	}
	
	// Write a tag indicating the section has ended
	p_out.write(reinterpret_cast<char *>(&section_end_tag), sizeof section_end_tag);
	
	// Genomes section
	bool use_16_bit = (mutation_map_size <= UINT16_MAX - 1);	// 0xFFFF is reserved as the start of our various tags
	
	for (const std::pair<const slim_objectid_t,Subpopulation*> &subpop_pair : *this)			// go through all subpopulations
	{
		Subpopulation *subpop = subpop_pair.second;
		slim_objectid_t subpop_id = subpop_pair.first;
		slim_popsize_t subpop_size = (child_generation_valid_ ? subpop->child_subpop_size_ : subpop->parent_subpop_size_);
		
		for (slim_popsize_t i = 0; i < 2 * subpop_size; i++)							// go through all children
		{
			Genome &genome = child_generation_valid_ ? subpop->child_genomes_[i] : subpop->parent_genomes_[i];
			
			// Write out the genome header; start with the genome type to guarantee that the first 32 bits are != section_end_tag
			int32_t genome_type = (int32_t)(genome.Type());
			
			p_out.write(reinterpret_cast<char *>(&genome_type), sizeof genome_type);
			p_out.write(reinterpret_cast<char *>(&subpop_id), sizeof subpop_id);
			p_out.write(reinterpret_cast<char *>(&i), sizeof i);
			
			// Output individual spatial position information before the mutation list.  Added in version 3.
			if (spatial_output_count && ((i % 2) == 0))
			{
				int individual_index = i / 2;
				Individual &individual = (child_generation_valid_ ? subpop->child_individuals_[individual_index] : subpop->parent_individuals_[individual_index]);
				
				if (spatial_output_count >= 1)
					p_out.write(reinterpret_cast<char *>(&individual.spatial_x_), sizeof individual.spatial_x_);
				if (spatial_output_count >= 2)
					p_out.write(reinterpret_cast<char *>(&individual.spatial_y_), sizeof individual.spatial_y_);
				if (spatial_output_count >= 3)
					p_out.write(reinterpret_cast<char *>(&individual.spatial_z_), sizeof individual.spatial_z_);
			}
			
			// Write out the mutation list
			if (genome.IsNull())
			{
				// null genomes get a 32-bit flag value written instead of a mutation count
				int32_t null_genome_tag = 0xFFFF1000;
				
				p_out.write(reinterpret_cast<char *>(&null_genome_tag), sizeof null_genome_tag);
			}
			else
			{
				// write a 32-bit mutation count
				{
					int32_t total_mutations = genome.mutation_count();
					
					p_out.write(reinterpret_cast<char *>(&total_mutations), sizeof total_mutations);
				}
				
				if (use_16_bit)
				{
					// Write out 16-bit mutation tags
					int mutrun_count = genome.mutrun_count_;
					
					for (int run_index = 0; run_index < mutrun_count; ++run_index)
					{
						MutationRun *mutrun = genome.mutruns_[run_index].get();
						int mut_count = mutrun->size();
						const MutationIndex *mut_ptr = mutrun->begin_pointer_const();
						
						for (int mut_index = 0; mut_index < mut_count; ++mut_index)
						{
							slim_polymorphismid_t polymorphism_id = FindMutationInPolymorphismMap(polymorphisms, mut_block_ptr + mut_ptr[mut_index]);
							
							if (polymorphism_id == -1)
								EIDOS_TERMINATION << "ERROR (Population::PrintAllBinary): (internal error) polymorphism not found." << EidosTerminate();
							
							if (polymorphism_id <= UINT16_MAX - 1)
							{
								uint16_t id_16 = (uint16_t)polymorphism_id;
								
								p_out.write(reinterpret_cast<char *>(&id_16), sizeof id_16);
							}
							else
							{
								EIDOS_TERMINATION << "ERROR (Population::PrintAllBinary): (internal error) mutation id out of 16-bit bounds." << EidosTerminate();
							}
						}
					}
				}
				else
				{
					// Write out 32-bit mutation tags
					int mutrun_count = genome.mutrun_count_;
					
					for (int run_index = 0; run_index < mutrun_count; ++run_index)
					{
						MutationRun *mutrun = genome.mutruns_[run_index].get();
						int mut_count = mutrun->size();
						const MutationIndex *mut_ptr = mutrun->begin_pointer_const();
						
						for (int mut_index = 0; mut_index < mut_count; ++mut_index)
						{
							slim_polymorphismid_t polymorphism_id = FindMutationInPolymorphismMap(polymorphisms, mut_block_ptr + mut_ptr[mut_index]);
							
							if (polymorphism_id == -1)
								EIDOS_TERMINATION << "ERROR (Population::PrintAllBinary): (internal error) polymorphism not found." << EidosTerminate();
							
							p_out.write(reinterpret_cast<char *>(&polymorphism_id), sizeof polymorphism_id);
						}
					}
				}
				
				// now will come either a genome type (32 bits: 0, 1, or 2), or a section end tag
			}
		}
	}
	
	// Write a tag indicating the section has ended
	p_out.write(reinterpret_cast<char *>(&section_end_tag), sizeof section_end_tag);
}

// print sample of p_sample_size genomes from subpopulation p_subpop_id
void Population::PrintSample_SLiM(std::ostream &p_out, Subpopulation &p_subpop, slim_popsize_t p_sample_size, bool p_replace, IndividualSex p_requested_sex) const
{
	// This function is written to be able to print the population whether child_generation_valid is true or false.
	
	std::vector<Genome> &subpop_genomes = (child_generation_valid_ ? p_subpop.child_genomes_ : p_subpop.parent_genomes_);
	slim_popsize_t subpop_size = (child_generation_valid_ ? p_subpop.child_subpop_size_ : p_subpop.parent_subpop_size_);
	
	if (p_requested_sex == IndividualSex::kFemale && p_subpop.modeled_chromosome_type_ == GenomeType::kYChromosome)
		EIDOS_TERMINATION << "ERROR (Population::PrintSample_SLiM): called to output Y chromosomes from females." << EidosTerminate();
	
	// assemble a sample (with or without replacement)
	std::vector<slim_popsize_t> candidates;
	
	for (slim_popsize_t s = subpop_size * 2 - 1; s >= 0; --s)
		candidates.emplace_back(s);
	
	std::vector<Genome *> sample; 
	
	for (slim_popsize_t s = 0; s < p_sample_size; s++)
	{
		int candidate_index;
		slim_popsize_t genome_index;
		
		// Scan for a genome that is not null and that belongs to an individual of the requested sex
		do {
			// select a random genome (not a random individual) by selecting a random candidate entry
			if (candidates.size() == 0)
				EIDOS_TERMINATION << "ERROR (Population::PrintSample_SLiM): not enough eligible genomes for sampling without replacement." << EidosTerminate();
			
			candidate_index = static_cast<slim_popsize_t>(gsl_rng_uniform_int(gEidos_rng, candidates.size()));
			genome_index = candidates[candidate_index];
			
			// If we're sampling without replacement, remove the index we have just taken; either we will use it or it is invalid
			if (!p_replace)
			{
				candidates[candidate_index] = candidates.back();
				candidates.pop_back();
			}
		} while (subpop_genomes[genome_index].IsNull() || (p_subpop.sex_enabled_ && p_requested_sex != IndividualSex::kUnspecified && p_subpop.SexOfIndividual(genome_index / 2) != p_requested_sex));
		
		sample.push_back(&subpop_genomes[genome_index]);
	}
	
	// print the sample using Genome's static member function
	Genome::PrintGenomes_SLiM(p_out, sample, p_subpop.subpopulation_id_);
}

// print sample of p_sample_size genomes from subpopulation p_subpop_id, using "ms" format
void Population::PrintSample_MS(std::ostream &p_out, Subpopulation &p_subpop, slim_popsize_t p_sample_size, bool p_replace, IndividualSex p_requested_sex, const Chromosome &p_chromosome) const
{
	// This function is written to be able to print the population whether child_generation_valid is true or false.
	
	std::vector<Genome> &subpop_genomes = (child_generation_valid_ ? p_subpop.child_genomes_ : p_subpop.parent_genomes_);
	slim_popsize_t subpop_size = (child_generation_valid_ ? p_subpop.child_subpop_size_ : p_subpop.parent_subpop_size_);
	
	if (p_requested_sex == IndividualSex::kFemale && p_subpop.modeled_chromosome_type_ == GenomeType::kYChromosome)
		EIDOS_TERMINATION << "ERROR (Population::PrintSample_MS): called to output Y chromosomes from females." << EidosTerminate();
	
	// assemble a sample (with or without replacement)
	std::vector<slim_popsize_t> candidates;
	
	for (slim_popsize_t s = subpop_size * 2 - 1; s >= 0; --s)
		candidates.emplace_back(s);
	
	std::vector<Genome *> sample; 
	
	for (slim_popsize_t s = 0; s < p_sample_size; s++)
	{
		int candidate_index;
		slim_popsize_t genome_index;
		
		// Scan for a genome that is not null and that belongs to an individual of the requested sex
		do {
			// select a random genome (not a random individual) by selecting a random candidate entry
			if (candidates.size() == 0)
				EIDOS_TERMINATION << "ERROR (Population::PrintSample_MS): not enough eligible genomes for sampling without replacement." << EidosTerminate();
			
			candidate_index = static_cast<slim_popsize_t>(gsl_rng_uniform_int(gEidos_rng, candidates.size()));
			genome_index = candidates[candidate_index];
			
			// If we're sampling without replacement, remove the index we have just taken; either we will use it or it is invalid
			if (!p_replace)
			{
				candidates[candidate_index] = candidates.back();
				candidates.pop_back();
			}
		} while (subpop_genomes[genome_index].IsNull() || (p_subpop.sex_enabled_ && p_requested_sex != IndividualSex::kUnspecified && p_subpop.SexOfIndividual(genome_index / 2) != p_requested_sex));
		
		sample.push_back(&subpop_genomes[genome_index]);
	}
	
	// print the sample using Genome's static member function
	Genome::PrintGenomes_MS(p_out, sample, p_chromosome);
}

// print sample of p_sample_size *individuals* (NOT genomes) from subpopulation p_subpop_id
void Population::PrintSample_VCF(std::ostream &p_out, Subpopulation &p_subpop, slim_popsize_t p_sample_size, bool p_replace, IndividualSex p_requested_sex, bool p_output_multiallelics) const
{
	// This function is written to be able to print the population whether child_generation_valid is true or false.
	
	std::vector<Genome> &subpop_genomes = (child_generation_valid_ ? p_subpop.child_genomes_ : p_subpop.parent_genomes_);
	slim_popsize_t subpop_size = (child_generation_valid_ ? p_subpop.child_subpop_size_ : p_subpop.parent_subpop_size_);
	
	if (p_requested_sex == IndividualSex::kFemale && p_subpop.modeled_chromosome_type_ == GenomeType::kYChromosome)
		EIDOS_TERMINATION << "ERROR (Population::PrintSample_VCF): called to output Y chromosomes from females." << EidosTerminate();
	if (p_requested_sex == IndividualSex::kUnspecified && p_subpop.modeled_chromosome_type_ == GenomeType::kYChromosome)
		EIDOS_TERMINATION << "ERROR (Population::PrintSample_VCF): called to output Y chromosomes from both sexes." << EidosTerminate();
	
	// assemble a sample (with or without replacement)
	std::vector<slim_popsize_t> candidates;
	
	for (slim_popsize_t s = subpop_size - 1; s >= 0; --s)
		candidates.emplace_back(s);
	
	std::vector<Genome *> sample; 
	
	for (slim_popsize_t s = 0; s < p_sample_size; s++)
	{
		int candidate_index;
		slim_popsize_t individual_index;
		slim_popsize_t genome1, genome2;
		
		// Scan for an individual of the requested sex
		do {
			// select a random genome (not a random individual) by selecting a random candidate entry
			if (candidates.size() == 0)
				EIDOS_TERMINATION << "ERROR (Population::PrintSample_VCF): not enough eligible individuals for sampling without replacement." << EidosTerminate();
			
			candidate_index = static_cast<slim_popsize_t>(gsl_rng_uniform_int(gEidos_rng, candidates.size()));
			individual_index = candidates[candidate_index];
			
			// If we're sampling without replacement, remove the index we have just taken; either we will use it or it is invalid
			if (!p_replace)
			{
				candidates[candidate_index] = candidates.back();
				candidates.pop_back();
			}
		} while (p_subpop.sex_enabled_ && (p_requested_sex != IndividualSex::kUnspecified) && (p_subpop.SexOfIndividual(individual_index) != p_requested_sex));
		
		genome1 = individual_index * 2;
		genome2 = genome1 + 1;
		
		sample.push_back(&subpop_genomes[genome1]);
		sample.push_back(&subpop_genomes[genome2]);
	}
	
	// print the sample using Genome's static member function
	Genome::PrintGenomes_VCF(p_out, sample, p_output_multiallelics);
}






































































