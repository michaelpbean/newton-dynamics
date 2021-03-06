/* Copyright (c) <2003-2016> <Julio Jerez, Newton Game Dynamics>
* 
* This software is provided 'as-is', without any express or implied
* warranty. In no event will the authors be held liable for any damages
* arising from the use of this software.
* 
* Permission is granted to anyone to use this software for any purpose,
* including commercial applications, and to alter it and redistribute it
* freely, subject to the following restrictions:
* 
* 1. The origin of this software must not be misrepresented; you must not
* claim that you wrote the original software. If you use this software
* in a product, an acknowledgment in the product documentation would be
* appreciated but is not required.
* 
* 2. Altered source versions must be plainly marked as such, and must not be
* misrepresented as being the original software.
* 
* 3. This notice may not be removed or altered from any source distribution.
*/

#include "dgPhysicsStdafx.h"
#include "dgBody.h"
#include "dgWorld.h"
#include "dgConstraint.h"
#include "dgDynamicBody.h"
#include "dgDynamicBody.h"
#include "dgSkeletonContainer.h"
#include "dgCollisionInstance.h"
#include "dgWorldDynamicUpdate.h"
#include "dgBilateralConstraint.h"

//#define DG_OLD_SOLVER

dgMatrix dgWorldDynamicUpdate::m_vectorMasks (dgVector (dgInt32 (0xffffffff), dgInt32 (0xffffffff), dgInt32 (0xffffffff), dgInt32 (0xffffffff)),
											  dgVector (dgInt32 (0xffffffff), dgInt32 (0xffffffff), dgInt32 (0xffffffff),		   dgInt32 (0)),
											  dgVector (dgInt32 (0xffffffff), dgInt32 (0xffffffff),			 dgInt32 (0),          dgInt32 (0)),
											  dgVector (dgInt32 (0xffffffff),		   dgInt32 (0),			 dgInt32 (0),          dgInt32 (0)));


void dgWorldDynamicUpdate::CalculateIslandReactionForces (dgIsland* const island, dgFloat32 timestep, dgInt32 threadID) const
{
	if (island->m_jointCount) {
		SortIsland(island, timestep, threadID);
	}

	if (!island->m_isContinueCollision) {
		if (island->m_jointCount) {
			BuildJacobianMatrix (island, threadID, timestep);
			CalculateReactionsForces (island, threadID, timestep, DG_SOLVER_MAX_ERROR);
		} else {
			IntegrateForce(island, timestep, threadID);
		}
		IntegrateVelocity (island, DG_SOLVER_MAX_ERROR, timestep, threadID); 
	} else {
		// calculate reaction forces and new velocities
		BuildJacobianMatrix (island, threadID, timestep);
		CalculateReactionsForces (island, threadID, timestep, DG_SOLVER_MAX_ERROR);

		// see if the island goes to sleep
		bool isAutoSleep = true;
		bool stackSleeping = true;
		dgInt32 sleepCounter = 10000;

		dgWorld* const world = (dgWorld*) this;
		const dgInt32 bodyCount = island->m_bodyCount;
		dgBodyInfo* const bodyArrayPtr = (dgBodyInfo*) &world->m_bodiesMemory[0]; 
		dgBodyInfo* const bodyArray = &bodyArrayPtr[island->m_bodyStart];

		const dgFloat32 forceDamp = DG_FREEZZING_VELOCITY_DRAG;
		dgFloat32 maxAccel = dgFloat32 (0.0f);
		dgFloat32 maxAlpha = dgFloat32 (0.0f);
		dgFloat32 maxSpeed = dgFloat32 (0.0f);
		dgFloat32 maxOmega = dgFloat32 (0.0f);

		const dgFloat32 speedFreeze = world->m_freezeSpeed2;
		const dgFloat32 accelFreeze = world->m_freezeAccel2;
		const dgVector forceDampVect (forceDamp, forceDamp, forceDamp, dgFloat32 (0.0f));
		for (dgInt32 i = 1; i < bodyCount; i ++) {
			dgDynamicBody* const body = (dgDynamicBody*) bodyArray[i].m_body;
			if (body->IsRTTIType (dgBody::m_dynamicBodyRTTI)) {
				dgAssert (body->m_invMass.m_w);

				const dgFloat32 accel2 = body->m_accel.DotProduct3(body->m_accel);
				const dgFloat32 alpha2 = body->m_alpha.DotProduct3(body->m_alpha);
				const dgFloat32 speed2 = body->m_veloc.DotProduct3(body->m_veloc);
				const dgFloat32 omega2 = body->m_omega.DotProduct3(body->m_omega);

				maxAccel = dgMax (maxAccel, accel2);
				maxAlpha = dgMax (maxAlpha, alpha2);
				maxSpeed = dgMax (maxSpeed, speed2);
				maxOmega = dgMax (maxOmega, omega2);

				bool equilibrium = (accel2 < accelFreeze) && (alpha2 < accelFreeze) && (speed2 < speedFreeze) && (omega2 < speedFreeze);
				if (equilibrium) {
					dgVector veloc (body->m_veloc.CompProduct4(forceDampVect));
					dgVector omega = body->m_omega.CompProduct4 (forceDampVect);
					body->m_veloc = (dgVector (veloc.DotProduct4(veloc)) > m_velocTol) & veloc;
					body->m_omega = (dgVector (omega.DotProduct4(omega)) > m_velocTol) & omega;

				}
				body->m_equilibrium = dgUnsigned32 (equilibrium);
				stackSleeping &= equilibrium;
				isAutoSleep &= body->m_autoSleep;

				sleepCounter = dgMin (sleepCounter, body->m_sleepingCounter);

				// clear force and torque accumulators
				body->m_accel = dgVector::m_zero;
				body->m_alpha = dgVector::m_zero;
			}
		}

		if (isAutoSleep) {
			if (stackSleeping) {
				// the island went to sleep mode, 
				for (dgInt32 i = 1; i < bodyCount; i ++) {
					dgDynamicBody* const body = (dgDynamicBody*) bodyArray[i].m_body;
					if (body->IsRTTIType (dgBody::m_dynamicBodyRTTI)) {
						body->m_netForce = dgVector::m_zero;
						body->m_netTorque = dgVector::m_zero;
						body->m_veloc = dgVector::m_zero;
						body->m_omega = dgVector::m_zero;
					}
				}
			} else {
				// island is not sleeping but may be resting with small residual velocity for a long time
				// see if we can force to go to sleep
				if ((maxAccel > world->m_sleepTable[DG_SLEEP_ENTRIES - 1].m_maxAccel) ||
					(maxAlpha > world->m_sleepTable[DG_SLEEP_ENTRIES - 1].m_maxAlpha) ||
					(maxSpeed > world->m_sleepTable[DG_SLEEP_ENTRIES - 1].m_maxVeloc) ||
					(maxOmega > world->m_sleepTable[DG_SLEEP_ENTRIES - 1].m_maxOmega)) {
					for (dgInt32 i = 1; i < bodyCount; i ++) {
						dgDynamicBody* const body = (dgDynamicBody*) bodyArray[i].m_body;
						if (body->IsRTTIType (dgBody::m_dynamicBodyRTTI)) {
							body->m_sleepingCounter = 0;
						}
					}
				} else {
					dgInt32 index = 0;
					for (dgInt32 i = 0; i < DG_SLEEP_ENTRIES; i ++) {
						if ((maxAccel <= world->m_sleepTable[i].m_maxAccel) &&
							(maxAlpha <= world->m_sleepTable[i].m_maxAlpha) &&
							(maxSpeed <= world->m_sleepTable[i].m_maxVeloc) &&
							(maxOmega <= world->m_sleepTable[i].m_maxOmega)) {
								index = i;
								break;
						}
					}

					dgInt32 timeScaleSleepCount = dgInt32 (dgFloat32 (60.0f) * sleepCounter * timestep);
					if (timeScaleSleepCount > world->m_sleepTable[index].m_steps) {
						// force island to sleep
						stackSleeping = true;
						for (dgInt32 i = 1; i < bodyCount; i ++) {
							dgDynamicBody* const body = (dgDynamicBody*) bodyArray[i].m_body;
							if (body->IsRTTIType (dgBody::m_dynamicBodyRTTI)) {
								body->m_netForce = dgVector::m_zero;
								body->m_netTorque = dgVector::m_zero;
								body->m_veloc = dgVector::m_zero;
								body->m_omega = dgVector::m_zero;
								body->m_equilibrium = true;
							}
						}
					} else {
						sleepCounter ++;
						for (dgInt32 i = 1; i < bodyCount; i ++) {
							dgDynamicBody* const body = (dgDynamicBody*) bodyArray[i].m_body;
							if (body->IsRTTIType (dgBody::m_dynamicBodyRTTI)) {
								body->m_sleepingCounter = sleepCounter;
							}
						}
					}
				}
			}
		} 


		if (!(isAutoSleep & stackSleeping)) {
			// island is not sleeping, need to integrate island velocity

			const dgUnsigned32 lru = world->GetBroadPhase()->m_lru;
			const dgInt32 jointCount = island->m_jointCount;
			dgJointInfo* const constraintArrayPtr = (dgJointInfo*) &world->m_jointsMemory[0];
			dgJointInfo* const constraintArray = &constraintArrayPtr[island->m_jointStart];

			dgFloat32 timeRemaining = timestep;
			const dgFloat32 timeTol = dgFloat32 (0.01f) * timestep;
			for (dgInt32 i = 0; (i < DG_MAX_CONTINUE_COLLISON_STEPS) && (timeRemaining > timeTol); i ++) {
//				dgAssert((i + 1) < DG_MAX_CONTINUE_COLLISON_STEPS);
				// calculate the closest time to impact 
				dgFloat32 timeToImpact = timeRemaining;
				for (dgInt32 j = 0; (j < jointCount) && (timeToImpact > timeTol); j ++) {
					dgContact* const contact = (dgContact*) constraintArray[j].m_joint;
					if (contact->GetId() == dgConstraint::m_contactConstraint) {
						dgDynamicBody* const body0 = (dgDynamicBody*)contact->m_body0;
						dgDynamicBody* const body1 = (dgDynamicBody*)contact->m_body1;
						if (body0->m_continueCollisionMode | body1->m_continueCollisionMode) {
							dgVector p;
							dgVector q;
							dgVector normal;
							timeToImpact = dgMin (timeToImpact, world->CalculateTimeToImpact (contact, timeToImpact, threadID, p, q, normal));
						}
					}
				}

				if (timeToImpact > timeTol) {
					timeRemaining -= timeToImpact;
					for (dgInt32 j = 1; j < bodyCount; j ++) {
						dgDynamicBody* const body = (dgDynamicBody*) bodyArray[j].m_body;
						if (body->IsRTTIType (dgBody::m_dynamicBodyRTTI)) {
							body->IntegrateVelocity(timeToImpact);
							body->UpdateWorlCollisionMatrix();
						}
					}
				} else {
					if (timeToImpact >= dgFloat32 (-1.0e-5f)) {
						for (dgInt32 j = 1; j < bodyCount; j++) {
							dgDynamicBody* const body = (dgDynamicBody*)bodyArray[j].m_body;
							if (body->IsRTTIType(dgBody::m_dynamicBodyRTTI)) {
								body->IntegrateVelocity(timeToImpact);
								body->UpdateWorlCollisionMatrix();
							}
						}
					}

					CalculateIslandContacts (island, timeRemaining, lru, threadID);
					BuildJacobianMatrix (island, threadID, 0.0f);
					CalculateReactionsForces (island, threadID, 0.0f, DG_SOLVER_MAX_ERROR);

					bool islandResinding = true;
					for (dgInt32 k = 0; (k < DG_MAX_CONTINUE_COLLISON_STEPS) && islandResinding; k ++) {
						dgFloat32 smallTimeStep = dgMin (timestep * dgFloat32 (1.0f / 8.0f), timeRemaining);
						timeRemaining -= smallTimeStep;
						for (dgInt32 j = 1; j < bodyCount; j ++) {
							dgDynamicBody* const body = (dgDynamicBody*) bodyArray[j].m_body;
							if (body->IsRTTIType (dgBody::m_dynamicBodyRTTI)) {
								body->IntegrateVelocity (smallTimeStep);
								body->UpdateWorlCollisionMatrix();
							}
						}

						islandResinding = false;
						if (timeRemaining > timeTol) {
							CalculateIslandContacts (island, timeRemaining, lru, threadID);

							bool isColliding = false;
							for (dgInt32 j = 0; (j < jointCount) && !isColliding; j ++) {
								dgContact* const contact = (dgContact*) constraintArray[j].m_joint;
								if (contact->GetId() == dgConstraint::m_contactConstraint) {

									const dgBody* const body0 = contact->m_body0;
									const dgBody* const body1 = contact->m_body1;

									const dgVector& veloc0 = body0->m_veloc;
									const dgVector& veloc1 = body1->m_veloc;

									const dgVector& omega0 = body0->m_omega;
									const dgVector& omega1 = body1->m_omega;

									const dgVector& com0 = body0->m_globalCentreOfMass;
									const dgVector& com1 = body1->m_globalCentreOfMass;
									
									for (dgList<dgContactMaterial>::dgListNode* node = contact->GetFirst(); node; node = node->GetNext()) {
										const dgContactMaterial* const contactMaterial = &node->GetInfo();
										dgVector vel0 (veloc0 + omega0.CrossProduct3(contactMaterial->m_point - com0));
										dgVector vel1 (veloc1 + omega1.CrossProduct3(contactMaterial->m_point - com1));
										dgVector vRel (vel0 - vel1);
										dgAssert (contactMaterial->m_normal.m_w == dgFloat32 (0.0f));
										dgFloat32 speed = vRel.DotProduct4(contactMaterial->m_normal).m_w;
										isColliding |= (speed < dgFloat32 (0.0f));
									}
								}
							}
							islandResinding = !isColliding;
						}
					}
				}
			}

			if (timeRemaining > dgFloat32 (0.0)) {
				for (dgInt32 j = 1; j < bodyCount; j ++) {
					dgDynamicBody* const body = (dgDynamicBody*) bodyArray[j].m_body;
					if (body->IsRTTIType (dgBody::m_dynamicBodyRTTI)) {
						body->IntegrateVelocity(timeRemaining);
						body->UpdateMatrix (timeRemaining, threadID);
					}
				}
			} else {
				for (dgInt32 j = 1; j < bodyCount; j ++) {
					dgDynamicBody* const body = (dgDynamicBody*) bodyArray[j].m_body;
					if (body->IsRTTIType (dgBody::m_dynamicBodyRTTI)) {
						body->UpdateMatrix (timestep, threadID);
					}
				}
			}
		}
	}
}


void dgWorldDynamicUpdate::CalculateIslandContacts (dgIsland* const island, dgFloat32 timestep, dgInt32 currLru, dgInt32 threadID) const
{
	dgWorld* const world = (dgWorld*) this;
	dgInt32 jointCount = island->m_jointCount;
	dgJointInfo* const constraintArrayPtr = (dgJointInfo*) &world->m_jointsMemory[0];
	dgJointInfo* const constraintArray = &constraintArrayPtr[island->m_jointStart];

	for (dgInt32 j = 0; (j < jointCount); j ++) {
		dgContact* const contact = (dgContact*) constraintArray[j].m_joint;
		if (contact->GetId() == dgConstraint::m_contactConstraint) {
			const dgContactMaterial* const material = contact->m_material;
			if (material->m_flags & dgContactMaterial::m_collisionEnable) {
				dgInt32 processContacts = 1;
				if (material->m_aabbOverlap) {
					processContacts = material->m_aabbOverlap (*material, *contact->GetBody0(), *contact->GetBody1(), threadID);
				}

				if (processContacts) {
					dgContactPoint contactArray[DG_MAX_CONTATCS];
					dgBroadPhase::dgPair pair;

					contact->m_maxDOF = 0;
					contact->m_broadphaseLru = currLru;
					pair.m_contact = contact;
					pair.m_cacheIsValid = false;
					pair.m_timestep = timestep;
					pair.m_contactBuffer = contactArray;
					world->CalculateContacts (&pair, threadID, false, false);
					if (pair.m_contactCount) {
						dgAssert (pair.m_contactCount <= (DG_CONSTRAINT_MAX_ROWS / 3));
						world->ProcessContacts (&pair, threadID);
					}
				}
			}
		}
	}
}


void dgWorldDynamicUpdate::BuildJacobianMatrix (const dgBodyInfo* const bodyInfoArray, const dgJointInfo* const jointInfo, dgJacobianMatrixElement* const matrixRow, dgFloat32 forceImpulseScale) const 
{
	if (jointInfo->m_joint->m_solverActive) {
		const dgInt32 index = jointInfo->m_pairStart;
		const dgInt32 count = jointInfo->m_pairCount;
		const dgInt32 m0 = jointInfo->m_m0;
		const dgInt32 m1 = jointInfo->m_m1;

	//	dgAssert(m0 >= 0);
	//	dgAssert(m0 < bodyCount);
	//	dgAssert(m1 >= 0);
	//	dgAssert(m1 < bodyCount);

		const dgBody* const body0 = bodyInfoArray[m0].m_body;
		const dgBody* const body1 = bodyInfoArray[m1].m_body;

		const dgVector invMass0(body0->m_invMass[3]);
		const dgMatrix& invInertia0 = body0->m_invWorldInertiaMatrix;
		const dgVector invMass1(body1->m_invMass[3]);
		const dgMatrix& invInertia1 = body1->m_invWorldInertiaMatrix;

		dgVector accel0(dgVector::m_zero);
		dgVector alpha0(dgVector::m_zero);
		if (body0->IsRTTIType(dgBody::m_dynamicBodyRTTI)) {
			accel0 = ((dgDynamicBody*)body0)->m_accel;
			alpha0 = ((dgDynamicBody*)body0)->m_alpha;
		}

		dgVector accel1(dgVector::m_zero);
		dgVector alpha1(dgVector::m_zero);
		if (body1->IsRTTIType(dgBody::m_dynamicBodyRTTI)) {
			accel1 = ((dgDynamicBody*)body1)->m_accel;
			alpha1 = ((dgDynamicBody*)body1)->m_alpha;
		}

		const dgVector scale0(jointInfo->m_scale0);
		const dgVector scale1(jointInfo->m_scale1);
		const dgInt32 vectorStride = dgInt32(sizeof(dgVector) / sizeof(dgFloat32));
		const dgInt32 blockStride = (count + vectorStride - 1) & (-vectorStride);
		dgFloat32 * const blokMatrix = &m_solverMemory.m_solverBlockJacobianMemory[jointInfo->m_blockMatrixStart];
		for (dgInt32 i = 0; i < count; i++) {
			dgFloat32 * const blokMatrixPtr = &blokMatrix[i * blockStride];
			dgJacobianMatrixElement* const row = &matrixRow[index + i];
			dgAssert(row->m_Jt.m_jacobianM0.m_linear.m_w == dgFloat32(0.0f));
			dgAssert(row->m_Jt.m_jacobianM0.m_angular.m_w == dgFloat32(0.0f));
			dgAssert(row->m_Jt.m_jacobianM1.m_linear.m_w == dgFloat32(0.0f));
			dgAssert(row->m_Jt.m_jacobianM1.m_angular.m_w == dgFloat32(0.0f));

			row->m_JMinv.m_jacobianM0.m_linear =  row->m_Jt.m_jacobianM0.m_linear.CompProduct4(invMass0);
			row->m_JMinv.m_jacobianM0.m_angular = invInertia0.RotateVector(row->m_Jt.m_jacobianM0.m_angular);
			row->m_JMinv.m_jacobianM1.m_linear =  row->m_Jt.m_jacobianM1.m_linear.CompProduct4(invMass1);
			row->m_JMinv.m_jacobianM1.m_angular = invInertia1.RotateVector(row->m_Jt.m_jacobianM1.m_angular);

			dgVector tmpAccel(row->m_JMinv.m_jacobianM0.m_linear.CompProduct4(accel0) + row->m_JMinv.m_jacobianM0.m_angular.CompProduct4(alpha0) +
							  row->m_JMinv.m_jacobianM1.m_linear.CompProduct4(accel1) + row->m_JMinv.m_jacobianM1.m_angular.CompProduct4(alpha1));

			dgAssert(tmpAccel.m_w == dgFloat32(0.0f));
			dgFloat32 extenalAcceleration = -(tmpAccel.AddHorizontal()).GetScalar();
			row->m_deltaAccel = extenalAcceleration * forceImpulseScale;
			row->m_coordenateAccel += extenalAcceleration * forceImpulseScale;
			dgAssert(row->m_jointFeebackForce);
			row->m_force = row->m_jointFeebackForce[0].m_force * forceImpulseScale;

			//force[index] = 0.0f;
			row->m_maxImpact = dgFloat32(0.0f);


			dgVector jMinvM0linear (scale0.CompProduct4(row->m_JMinv.m_jacobianM0.m_linear));
			dgVector jMinvM0angular (scale0.CompProduct4(row->m_JMinv.m_jacobianM0.m_angular));
			dgVector jMinvM1linear (scale1.CompProduct4(row->m_JMinv.m_jacobianM1.m_linear));
			dgVector jMinvM1angular (scale1.CompProduct4(row->m_JMinv.m_jacobianM1.m_angular));

			dgVector tmpDiag(jMinvM0linear.CompProduct4(row->m_Jt.m_jacobianM0.m_linear) + jMinvM0angular.CompProduct4(row->m_Jt.m_jacobianM0.m_angular) +
							 jMinvM1linear.CompProduct4(row->m_Jt.m_jacobianM1.m_linear) + jMinvM1angular.CompProduct4(row->m_Jt.m_jacobianM1.m_angular));

			dgAssert (tmpDiag.m_w == dgFloat32 (0.0f));
			dgFloat32 diag = (tmpDiag.AddHorizontal()).GetScalar();
			dgAssert(diag > dgFloat32(0.0f));
			row->m_diagDamp = diag * row->m_stiffness;
			diag *= (dgFloat32(1.0f) + row->m_stiffness);

			blokMatrixPtr[i] = diag;
			row->m_invJMinvJt = dgFloat32(1.0f) / diag;

			for (dgInt32 j = i + 1; j < count;  j++) {
				dgJacobianMatrixElement* const row_j = &matrixRow[index + j];
				dgVector tmpDiag(jMinvM0linear.CompProduct4(row_j->m_Jt.m_jacobianM0.m_linear) + jMinvM0angular.CompProduct4(row_j->m_Jt.m_jacobianM0.m_angular) +
								 jMinvM1linear.CompProduct4(row_j->m_Jt.m_jacobianM1.m_linear) + jMinvM1angular.CompProduct4(row_j->m_Jt.m_jacobianM1.m_angular));
				dgFloat32 diag = (tmpDiag.AddHorizontal()).GetScalar();
				blokMatrixPtr[j] = diag;
				blokMatrix[j * blockStride + i] = diag;
			}
			for (dgInt32 j = count; j < blockStride; j++) {
				blokMatrixPtr[j] = dgFloat32 (0.0f);
			}
		}
	}
}


void dgWorldDynamicUpdate::BuildJacobianMatrix (dgIsland* const island, dgInt32 threadIndex, dgFloat32 timestep) const 
{
	dTimeTrackerEvent(__FUNCTION__);
	dgAssert (island->m_bodyCount >= 2);

	dgWorld* const world = (dgWorld*) this;
	const dgInt32 bodyCount = island->m_bodyCount;
	const dgInt32 jointCount = island->m_jointCount;

	dgBodyInfo* const bodyArrayPtr = (dgBodyInfo*) &world->m_bodiesMemory[0]; 
	dgBodyInfo* const bodyArray = &bodyArrayPtr[island->m_bodyStart];
	dgJacobian* const internalForces = &m_solverMemory.m_internalForcesBuffer[island->m_bodyStart];

	dgAssert (((dgDynamicBody*) bodyArray[0].m_body)->IsRTTIType (dgBody::m_dynamicBodyRTTI));
	dgAssert ((((dgDynamicBody*)bodyArray[0].m_body)->m_accel.DotProduct3(((dgDynamicBody*)bodyArray[0].m_body)->m_accel)) == dgFloat32 (0.0f));
	dgAssert ((((dgDynamicBody*)bodyArray[0].m_body)->m_alpha.DotProduct3(((dgDynamicBody*)bodyArray[0].m_body)->m_alpha)) == dgFloat32 (0.0f));

	dgAssert(bodyArray[0].m_body->m_resting);
	internalForces[0].m_linear = dgVector::m_zero;
	internalForces[0].m_angular = dgVector::m_zero;

	if (timestep != dgFloat32 (0.0f)) {
		for (dgInt32 i = 1; i < bodyCount; i ++) {
			dgBody* const body = bodyArray[i].m_body;
			if (!body->m_equilibrium) {
				dgAssert (body->m_invMass.m_w > dgFloat32 (0.0f));
				body->AddDampingAcceleration(timestep);
				body->CalcInvInertiaMatrix ();
			}

			if (body->m_active) {
				// re use these variables for temp storage 
				body->m_netForce = body->m_veloc;
				body->m_netTorque = body->m_omega;

				internalForces[i].m_linear = dgVector::m_zero;
				internalForces[i].m_angular = dgVector::m_zero;
			}
		}

	} else {
		for (dgInt32 i = 1; i < bodyCount; i ++) {
			dgBody* const body = bodyArray[i].m_body;
			if (!body->m_equilibrium) {
				dgAssert (body->m_invMass.m_w > dgFloat32 (0.0f));
				body->CalcInvInertiaMatrix ();
			}
			if (body->m_active) {
				// re use these variables for temp storage 
				body->m_netForce = body->m_veloc;
				body->m_netTorque = body->m_omega;

				internalForces[i].m_linear = dgVector::m_zero;
				internalForces[i].m_angular = dgVector::m_zero;
			}
		}
	}
	
	if (jointCount) {
		dgInt32 rowCount = 0;
		dgJointInfo* const constraintArrayPtr = (dgJointInfo*) &world->m_jointsMemory[0];
		dgJointInfo* const constraintArray = &constraintArrayPtr[island->m_jointStart];

		GetJacobianDerivatives (island, threadIndex, rowCount, timestep);

		dgFloat32 forceOrImpulseScale = (timestep > dgFloat32 (0.0f)) ? dgFloat32 (1.0f) : dgFloat32 (0.0f);

		dgJacobianMatrixElement* const matrixRow = &m_solverMemory.m_jacobianBuffer[island->m_rowsStart];
		for (dgInt32 k = 0; k < jointCount; k ++) {
			const dgJointInfo* const jointInfo = &constraintArray[k];

			dgAssert(jointInfo->m_m0 >= 0);
			dgAssert(jointInfo->m_m0 < bodyCount);
			dgAssert(jointInfo->m_m1 >= 0);
			dgAssert(jointInfo->m_m1 < bodyCount);
			BuildJacobianMatrix (bodyArray, jointInfo, matrixRow, forceOrImpulseScale);
		}
	}
}

void dgWorldDynamicUpdate::IntegrateForce(const dgIsland* const island, dgFloat32 timestep, dgInt32 threadID) const
{
	dgWorld* const world = (dgWorld*) this;
	dgBodyInfo* const bodyArrayPtr = (dgBodyInfo*) &world->m_bodiesMemory[0]; 
	dgBodyInfo* const bodyArray = &bodyArrayPtr[island->m_bodyStart];

	dgVector timeStepVect (timestep);
	dgAssert (timestep > dgFloat32 (0.0f));
	dgDynamicBody* const body = (dgDynamicBody*) bodyArray[1].m_body;
	dgVector accel (body->m_accel.Scale4 (body->m_invMass.m_w));
	dgVector alpha (body->m_invWorldInertiaMatrix.RotateVector (body->m_alpha));

	body->m_netForce = body->m_accel;
	body->m_netTorque = body->m_alpha;

	body->m_veloc += accel.CompProduct4(timeStepVect);
	dgVector correction (alpha.CrossProduct3(body->m_omega));
	body->m_omega += alpha.CompProduct4(timeStepVect.CompProduct4 (dgVector::m_half)) + correction.CompProduct4(timeStepVect.CompProduct4(timeStepVect.CompProduct4 (m_eulerTaylorCorrection)));
}

void dgWorldDynamicUpdate::InitJointForce(dgJointInfo* const jointInfo, dgJacobianMatrixElement* const matrixRow, dgJacobian& force0, dgJacobian& force1) const
{
	force0.m_linear = dgVector::m_zero;
	force0.m_angular = dgVector::m_zero;
	force1.m_linear = dgVector::m_zero;
	force1.m_angular = dgVector::m_zero;
	const dgInt32 first = jointInfo->m_pairStart;
	const dgInt32 count = jointInfo->m_pairCount;

	for (dgInt32 j = 0; j < count; j++) {
		dgJacobianMatrixElement* const row = &matrixRow[j + first];
		dgAssert(dgCheckFloat(row->m_force));
		dgVector val(row->m_force);
		force0.m_linear += row->m_Jt.m_jacobianM0.m_linear.CompProduct4(val);
		force0.m_angular += row->m_Jt.m_jacobianM0.m_angular.CompProduct4(val);
		force1.m_linear += row->m_Jt.m_jacobianM1.m_linear.CompProduct4(val);
		force1.m_angular += row->m_Jt.m_jacobianM1.m_angular.CompProduct4(val);
	}

	const dgVector scale0(jointInfo->m_scale0);
	const dgVector scale1(jointInfo->m_scale1);
	force0.m_linear = force0.m_linear.CompProduct4(scale0);
	force0.m_angular = force0.m_angular.CompProduct4(scale0);
	force1.m_linear = force1.m_linear.CompProduct4(scale1);
	force1.m_angular = force1.m_angular.CompProduct4(scale1);
}

void dgWorldDynamicUpdate::ApplyNetVelcAndOmega(dgDynamicBody* const body, const dgJacobian& forceAndTorque, const dgVector& timestep4, const dgVector& speedFreeze2, const dgVector& forceActiveMask) const
{
	if (body->m_active) {
		dgVector force(forceAndTorque.m_linear);
		dgVector torque(forceAndTorque.m_angular);
		if (body->IsRTTIType(dgBody::m_dynamicBodyRTTI)) {
			force += body->m_accel;
			torque += body->m_alpha;
		}

#if 1
		// this method is more accurate numerically 
		dgVector velocStep((force.Scale4(body->m_invMass.m_w)).CompProduct4(timestep4));
		dgVector omegaStep((body->m_invWorldInertiaMatrix.RotateVector(torque)).CompProduct4(timestep4));
		if (!body->m_resting) {
			body->m_veloc += velocStep;
			body->m_omega += omegaStep;
		} else {
			dgVector velocStep2(velocStep.DotProduct4(velocStep));
			dgVector omegaStep2(omegaStep.DotProduct4(omegaStep));
			dgVector test((velocStep2 > speedFreeze2) | (omegaStep2 > speedFreeze2) | forceActiveMask);
			if (test.GetSignMask()) {
				body->m_resting = false;
			}
		}

#else
		// this method is Lagrange-Euler by is more expensive and accumulate more numerical error. 
		dgVector linearMomentum(force.CompProduct4(timestep4) + body->m_veloc.Scale4(body->m_mass.m_w));
		dgVector angularMomentum(torque.CompProduct4(timestep4) + body->m_matrix.RotateVector(body->m_mass.CompProduct4(body->m_matrix.UnrotateVector(body->m_omega))));
		if (!body->m_resting) {
			body->m_veloc = linearMomentum.Scale4(body->m_invMass.m_w);
			body->m_omega = body->m_invWorldInertiaMatrix.RotateVector(angularMomentum);
		}
		else {
			dgVector velocStep(linearMomentum.Scale4(body->m_invMass.m_w));
			dgVector omegaStep(body->m_invWorldInertiaMatrix.RotateVector(angularMomentum));
			dgVector velocStep2(velocStep.DotProduct4(velocStep));
			dgVector omegaStep2(omegaStep.DotProduct4(omegaStep));
			dgVector test((velocStep2 > speedFreeze2) | (omegaStep2 > speedFreeze2) | forceActiveMask);
			if (test.GetSignMask()) {
				body->m_resting = false;
			}
		}
#endif
	}

	dgAssert(body->m_veloc.m_w == dgFloat32(0.0f));
	dgAssert(body->m_omega.m_w == dgFloat32(0.0f));
}

void dgWorldDynamicUpdate::ApplyNetTorqueAndForce(dgDynamicBody* const body, const dgVector& invTimeStep, const dgVector& maxAccNorm2, const dgVector& forceActiveMask) const
{
	if (body->m_active) {
		// the initial velocity and angular velocity were stored in net force and net torque, for memory saving
		dgVector accel = (body->m_veloc - body->m_netForce).CompProduct4(invTimeStep);
		dgVector alpha = (body->m_omega - body->m_netTorque).CompProduct4(invTimeStep);
		dgVector accelTest((accel.DotProduct4(accel) > maxAccNorm2) | (alpha.DotProduct4(alpha) > maxAccNorm2) | forceActiveMask);
		//if ((accel % accel) < maxAccNorm2) {
		//	accel = dgVector::m_zero;
		//}
		//if ((alpha % alpha) < maxAccNorm2) {
		//	alpha = dgVector::m_zero;
		//}
		accel = accel & accelTest;
		alpha = alpha & accelTest;

		if (body->IsRTTIType(dgBody::m_dynamicBodyRTTI)) {
			body->m_accel = accel;
			body->m_alpha = alpha;
		}
		body->m_netForce = accel.Scale4(body->m_mass[3]);

		alpha = body->m_matrix.UnrotateVector(alpha);
		body->m_netTorque = body->m_matrix.RotateVector(alpha.CompProduct4(body->m_mass));
	}
}


void dgWorldDynamicUpdate::CalculateReactionsForces(const dgIsland* const island, dgInt32 threadIndex, dgFloat32 timestep, dgFloat32 maxAccNorm) const
{
	if (island->m_jointCount == 0) {
		IntegrateForce(island, timestep, threadIndex);
	} else {
		CalculateForcesGameMode(island, threadIndex, timestep, maxAccNorm);
	}
}


dgFloat32 dgWorldDynamicUpdate::CalculateJointForce(const dgJointInfo* const jointInfo, const dgBodyInfo* const bodyArray, dgJacobian* const internalForces, dgJacobianMatrixElement* const matrixRow) const
{
	dgVector accNorm(dgVector::m_zero);

	dgConstraint* const constraint = jointInfo->m_joint;
	if (constraint->m_solverActive) {
		const dgInt32 m0 = jointInfo->m_m0;
		const dgInt32 m1 = jointInfo->m_m1;
		const dgBody* const body0 = bodyArray[m0].m_body;
		const dgBody* const body1 = bodyArray[m1].m_body;

		if (!(body0->m_resting & body1->m_resting)) {
			dgVector linearM0(internalForces[m0].m_linear);
			dgVector angularM0(internalForces[m0].m_angular);
			dgVector linearM1(internalForces[m1].m_linear);
			dgVector angularM1(internalForces[m1].m_angular);
			dgVector low[DG_CONSTRAINT_MAX_ROWS];
			dgVector high[DG_CONSTRAINT_MAX_ROWS];
			dgVector x[DG_CONSTRAINT_MAX_ROWS >> 2];
			dgVector b[DG_CONSTRAINT_MAX_ROWS];
			dgFloat32 diag[DG_CONSTRAINT_MAX_ROWS];
			dgFloat32 diagInv[DG_CONSTRAINT_MAX_ROWS];
			dgFloat32 cacheForce[DG_CONSTRAINT_MAX_ROWS + 4];
			cacheForce[0] = dgFloat32(1.0f);
			cacheForce[1] = dgFloat32(1.0f);
			cacheForce[2] = dgFloat32(1.0f);
			cacheForce[3] = dgFloat32(1.0f);
			dgFloat32* const normalForce = &cacheForce[4];

			const dgInt32 index = jointInfo->m_pairStart;
			const dgInt32 rowsCount = jointInfo->m_pairCount;
			const dgInt32 vectorStride = dgInt32(sizeof(dgVector) / sizeof(dgFloat32));
			const dgInt32 blockStride = (rowsCount + vectorStride - 1) & (-vectorStride);
			const dgInt32 simdBlockStride = blockStride >> 2;
			const dgFloat32* const blokMatrix = &m_solverMemory.m_solverBlockJacobianMemory[jointInfo->m_blockMatrixStart];

			dgFloat32* const x_ptr = &x[0].m_x;
			for (dgInt32 i = 0; i < rowsCount; i++) {
				dgJacobianMatrixElement* const row = &matrixRow[index + i];
				const dgFloat32* const blokMatrixRow = &blokMatrix[blockStride * i];

				dgAssert(row->m_Jt.m_jacobianM0.m_linear.m_w == dgFloat32(0.0f));
				dgAssert(row->m_Jt.m_jacobianM0.m_angular.m_w == dgFloat32(0.0f));
				dgAssert(row->m_Jt.m_jacobianM1.m_linear.m_w == dgFloat32(0.0f));
				dgAssert(row->m_Jt.m_jacobianM1.m_angular.m_w == dgFloat32(0.0f));

				dgVector a(row->m_JMinv.m_jacobianM0.m_linear.CompProduct4(linearM0) + row->m_JMinv.m_jacobianM0.m_angular.CompProduct4(angularM0) +
						   row->m_JMinv.m_jacobianM1.m_linear.CompProduct4(linearM1) + row->m_JMinv.m_jacobianM1.m_angular.CompProduct4(angularM1));

				dgInt32 frictionIndex = row->m_normalForceIndex;
				dgAssert(((frictionIndex < 0) && (normalForce[frictionIndex] == dgFloat32(1.0f))) || ((frictionIndex >= 0) && (normalForce[frictionIndex] >= dgFloat32(0.0f))));
				dgFloat32 frictionNormal = normalForce[frictionIndex];

				x_ptr[i] = dgFloat32(0.0f);
				normalForce[i] = row->m_force;
				b[i] = dgVector(row->m_coordenateAccel - row->m_force * row->m_diagDamp) - a.AddHorizontal();
				diag[i] = blokMatrixRow[i];
				diagInv[i] = row->m_invJMinvJt;
				low[i] = dgVector(frictionNormal * row->m_lowerBoundFrictionCoefficent - row->m_force);
				high[i] = dgVector(frictionNormal * row->m_upperBoundFrictionCoefficent - row->m_force);
			}

			const dgInt32 maskIndex = blockStride - rowsCount;
			const dgInt32 lastVectorElement = simdBlockStride - 1;
			const dgVector& mask = m_vectorMasks[maskIndex];
			x[lastVectorElement] = x[lastVectorElement] & mask;

			for (dgInt32 i = 0; i < rowsCount; i++) {
				const dgVector* const blokMatrixRow = (dgVector*)&blokMatrix[blockStride * i];
				dgVector accel(dgVector::m_zero);
				for (dgInt32 j = 0; j < simdBlockStride; j++) {
					accel += x[j].CompProduct4(blokMatrixRow[j]);
				}
				accel = b[i] - accel.AddHorizontal();
				dgVector force((accel.GetScalar() + diag[i] * x_ptr[i]) * diagInv[i]);
				accel = accel.AndNot((force > high[i]) | (force < low[i]));
				force = force.GetMax(low[i]).GetMin(high[i]);
				x_ptr[i] = force.GetScalar();
				accNorm = accNorm.GetMax(accel.Abs());
			}

			if (accNorm.GetScalar() > dgFloat32(5.0e-2f)) {
				dgVector maxAccel(accNorm);
				dgVector scale(dgFloat32 (1.2f));

				const dgInt32 steps = 5;
				dgFloat32 prevError = dgFloat32(1.0e20f);
				
				switch (simdBlockStride - 1)
				{
					case 0:
					{
						for (dgInt32 k = 0; (k < steps) && (maxAccel.GetScalar() > m_solverConvergeQuality) && (dgAbsf(prevError - maxAccel.GetScalar()) > dgFloat32(5.0e-2f)); k++) {
							prevError = maxAccel.GetScalar();
							maxAccel = dgVector::m_zero;
							for (dgInt32 i = 0; i < rowsCount; i++) {
								const dgVector* const blokMatrixRow = (dgVector*)&blokMatrix[blockStride * i];
								dgVector accel(x[0].CompProduct4(blokMatrixRow[0]));
								accel = scale.CompProduct4(b[i] - accel.AddHorizontal());
								dgVector force((accel.GetScalar() + diag[i] * x_ptr[i]) * diagInv[i]);
								accel = accel.AndNot((force > high[i]) | (force < low[i]));
								force = force.GetMax(low[i]).GetMin(high[i]);
								x_ptr[i] = force.GetScalar();
								maxAccel = maxAccel.GetMax(accel.Abs());
							}
						}
						break;
					}

					case 1:
					{
						for (dgInt32 k = 0; (k < steps) && (maxAccel.GetScalar() > m_solverConvergeQuality) && (dgAbsf(prevError - maxAccel.GetScalar()) > dgFloat32(5.0e-2f)); k++) {
							prevError = maxAccel.GetScalar();
							maxAccel = dgVector::m_zero;
							for (dgInt32 i = 0; i < rowsCount; i++) {
								const dgVector* const blokMatrixRow = (dgVector*)&blokMatrix[blockStride * i];
								dgVector accel(x[0].CompProduct4(blokMatrixRow[0]) + x[1].CompProduct4(blokMatrixRow[1]));
								accel = scale.CompProduct4(b[i] - accel.AddHorizontal());
								dgVector force((accel.GetScalar() + diag[i] * x_ptr[i]) * diagInv[i]);
								accel = accel.AndNot((force > high[i]) | (force < low[i]));
								force = force.GetMax(low[i]).GetMin(high[i]);
								x_ptr[i] = force.GetScalar();
								maxAccel = maxAccel.GetMax(accel.Abs());
							}
						}
						break;
					}

					case 2:
					{
						for (dgInt32 k = 0; (k < steps) && (maxAccel.GetScalar() > m_solverConvergeQuality) && (dgAbsf(prevError - maxAccel.GetScalar()) > dgFloat32(5.0e-2f)); k++) {
							prevError = maxAccel.GetScalar();
							maxAccel = dgVector::m_zero;
							for (dgInt32 i = 0; i < rowsCount; i++) {
								const dgVector* const blokMatrixRow = (dgVector*)&blokMatrix[blockStride * i];
								dgVector accel(x[0].CompProduct4(blokMatrixRow[0]) + x[1].CompProduct4(blokMatrixRow[1]) + x[2].CompProduct4(blokMatrixRow[2]));
								accel = scale.CompProduct4(b[i] - accel.AddHorizontal());
								dgVector force((accel.GetScalar() + diag[i] * x_ptr[i]) * diagInv[i]);
								accel = accel.AndNot((force > high[i]) | (force < low[i]));
								force = force.GetMax(low[i]).GetMin(high[i]);
								x_ptr[i] = force.GetScalar();
								maxAccel = maxAccel.GetMax(accel.Abs());
							}
						}
						break;
					}

					case 3:
					{
						for (dgInt32 k = 0; (k < steps) && (maxAccel.GetScalar() > m_solverConvergeQuality) && (dgAbsf(prevError - maxAccel.GetScalar()) > dgFloat32(5.0e-2f)); k++) {
							prevError = maxAccel.GetScalar();
							maxAccel = dgVector::m_zero;
							for (dgInt32 i = 0; i < rowsCount; i++) {
								const dgVector* const blokMatrixRow = (dgVector*)&blokMatrix[blockStride * i];
								dgVector accel(x[0].CompProduct4(blokMatrixRow[0]) + x[1].CompProduct4(blokMatrixRow[1]) + x[2].CompProduct4(blokMatrixRow[2]) + x[3].CompProduct4(blokMatrixRow[3]));
								accel = scale.CompProduct4(b[i] - accel.AddHorizontal());
								dgVector force((accel.GetScalar() + diag[i] * x_ptr[i]) * diagInv[i]);
								accel = accel.AndNot((force > high[i]) | (force < low[i]));
								force = force.GetMax(low[i]).GetMin(high[i]);
								x_ptr[i] = force.GetScalar();
								maxAccel = maxAccel.GetMax(accel.Abs());
							}
						}
						break;
					}

					case 4:
					{
						for (dgInt32 k = 0; (k < steps) && (maxAccel.GetScalar() > m_solverConvergeQuality) && (dgAbsf(prevError - maxAccel.GetScalar()) > dgFloat32(5.0e-2f)); k++) {
							prevError = maxAccel.GetScalar();
							maxAccel = dgVector::m_zero;
							for (dgInt32 i = 0; i < rowsCount; i++) {
								const dgVector* const blokMatrixRow = (dgVector*)&blokMatrix[blockStride * i];
								dgVector accel(x[0].CompProduct4(blokMatrixRow[0]) + x[1].CompProduct4(blokMatrixRow[1]) + x[2].CompProduct4(blokMatrixRow[2]) + x[3].CompProduct4(blokMatrixRow[3]) + x[4].CompProduct4(blokMatrixRow[4]));
								accel = scale.CompProduct4(b[i] - accel.AddHorizontal());
								dgVector force((accel.GetScalar() + diag[i] * x_ptr[i]) * diagInv[i]);
								accel = accel.AndNot((force > high[i]) | (force < low[i]));
								force = force.GetMax(low[i]).GetMin(high[i]);
								x_ptr[i] = force.GetScalar();
								maxAccel = maxAccel.GetMax(accel.Abs());
							}
						}
						break;
					}

					default:
					{
						for (dgInt32 k = 0; (k < steps) && (maxAccel.GetScalar() > m_solverConvergeQuality) && (dgAbsf(prevError - maxAccel.GetScalar()) > dgFloat32(5.0e-2f)); k++) {
							prevError = maxAccel.GetScalar();
							maxAccel = dgVector::m_zero;
							for (dgInt32 i = 0; i < rowsCount; i++) {
								const dgVector* const blokMatrixRow = (dgVector*)&blokMatrix[blockStride * i];
								dgVector accel(dgVector::m_zero);
								for (dgInt32 j = 0; j < simdBlockStride; j++) {
									accel += x[j].CompProduct4(blokMatrixRow[j]);
								}
								accel = scale.CompProduct4(b[i] - accel.AddHorizontal());
								dgVector force((accel.GetScalar() + diag[i] * x_ptr[i]) * diagInv[i]);
								accel = accel.AndNot((force > high[i]) | (force < low[i]));
								force = force.GetMax(low[i]).GetMin(high[i]);
								x_ptr[i] = force.GetScalar();
								maxAccel = maxAccel.GetMax(accel.Abs());
							}
						}
					}
				}
			} else {
				
			}

			const dgFloat32 scale0 = jointInfo->m_scale0;
			const dgFloat32 scale1(jointInfo->m_scale1);

			for (dgInt32 i = 0; i < rowsCount; i++) {
				dgJacobianMatrixElement* const row = &matrixRow[index + i];
				dgFloat32 deltaForce = x_ptr[i];
				row->m_force += deltaForce;
				dgVector jointForce0(deltaForce * scale0);
				dgVector jointForce1(deltaForce * scale1);
				linearM0 += row->m_Jt.m_jacobianM0.m_linear.CompProduct4(jointForce0);
				angularM0 += row->m_Jt.m_jacobianM0.m_angular.CompProduct4(jointForce0);
				linearM1 += row->m_Jt.m_jacobianM1.m_linear.CompProduct4(jointForce1);
				angularM1 += row->m_Jt.m_jacobianM1.m_angular.CompProduct4(jointForce1);
			}
			internalForces[m0].m_linear = linearM0;
			internalForces[m0].m_angular = angularM0;
			internalForces[m1].m_linear = linearM1;
			internalForces[m1].m_angular = angularM1;
		}
	}
	return accNorm.GetScalar();
}


void dgWorldDynamicUpdate::CalculateForcesGameMode(const dgIsland* const island, dgInt32 threadIndex, dgFloat32 timestep, dgFloat32 maxAccNorm) const
{
	dTimeTrackerEvent(__FUNCTION__);
	dgWorld* const world = (dgWorld*) this;
	const dgInt32 bodyCount = island->m_bodyCount;
	const dgInt32 jointCount = island->m_jointCount;

	dgJacobian* const internalForces = &m_solverMemory.m_internalForcesBuffer[island->m_bodyStart];
	dgBodyInfo* const bodyArrayPtr = (dgBodyInfo*)&world->m_bodiesMemory[0];
	dgJointInfo* const constraintArrayPtr = (dgJointInfo*)&world->m_jointsMemory[0];

	dgBodyInfo* const bodyArray = &bodyArrayPtr[island->m_bodyStart];
	dgJointInfo* const constraintArray = &constraintArrayPtr[island->m_jointStart];
	dgJacobianMatrixElement* const matrixRow = &m_solverMemory.m_jacobianBuffer[island->m_rowsStart];

	for (dgInt32 i = 0; i < jointCount; i++) {
		dgJointInfo* const jointInfo = &constraintArray[i];
		dgConstraint* const constraint = jointInfo->m_joint;
		constraint->m_index = i;
		if (constraint->m_solverActive) {
			dgJacobian y0;
			dgJacobian y1;
			InitJointForce(jointInfo, matrixRow, y0, y1);
			const dgInt32 m0 = jointInfo->m_m0;
			const dgInt32 m1 = jointInfo->m_m1;
			dgAssert(m0 != m1);
			internalForces[m0].m_linear += y0.m_linear;
			internalForces[m0].m_angular += y0.m_angular;
			internalForces[m1].m_linear += y1.m_linear;
			internalForces[m1].m_angular += y1.m_angular;
		}
	}


	const dgInt32 maxPasses = 4;

	dgFloat32 invTimestep = (timestep > dgFloat32(0.0f)) ? dgFloat32(1.0f) / timestep : dgFloat32(0.0f);
	dgFloat32 invStepRK = (dgFloat32(1.0f) / dgFloat32(maxPasses));
	dgFloat32 timestepRK = timestep * invStepRK;
	dgFloat32 invTimestepRK = invTimestep * dgFloat32(maxPasses);
	dgAssert(bodyArray[0].m_body == world->m_sentinelBody);

	dgVector speedFreeze2(world->m_freezeSpeed2 * dgFloat32(0.1f));
	dgVector freezeOmega2(world->m_freezeOmega2 * dgFloat32(0.1f));
	dgVector forceActiveMask((jointCount <= DG_SMALL_ISLAND_COUNT) ? dgVector(-1, -1, -1, -1) : dgFloat32(0.0f));

	dgJointAccelerationDecriptor joindDesc;
	joindDesc.m_timeStep = timestepRK;
	joindDesc.m_invTimeStep = invTimestepRK;
	joindDesc.m_firstPassCoefFlag = dgFloat32(0.0f);

	dgInt32 skeletonCount = 0;
	dgInt32 skeletonMemorySizeInBytes = 0;
	dgInt32 lru = dgAtomicExchangeAndAdd(&dgSkeletonContainer::m_lruMarker, 1);
	dgSkeletonContainer* skeletonArray[DG_MAX_SKELETON_JOINT_COUNT];
	dgInt32 memorySizes[DG_MAX_SKELETON_JOINT_COUNT];
	for (dgInt32 i = 1; i < bodyCount; i++) {
		dgDynamicBody* const body = (dgDynamicBody*)bodyArray[i].m_body;
		dgSkeletonContainer* const container = body->GetSkeleton();
		if (container && (container->m_lru != lru)) {
			container->m_lru = lru;
			memorySizes[skeletonCount] = container->CalculateMemoryBufferSizeInBytes(constraintArray, matrixRow);
			skeletonMemorySizeInBytes += memorySizes[skeletonCount];
			skeletonArray[skeletonCount] = container;
			skeletonCount++;
			dgAssert(skeletonCount < dgInt32(sizeof (skeletonArray) / sizeof (skeletonArray[0])));
		}
	}

	dgInt8* const skeletonMemory = (dgInt8*)dgAlloca(dgVector, skeletonMemorySizeInBytes / sizeof (dgVector));
	dgAssert((dgInt64(skeletonMemory) & 0x0f) == 0);

	skeletonMemorySizeInBytes = 0;
	for (dgInt32 i = 0; i < skeletonCount; i++) {
		skeletonArray[i]->InitMassMatrix(constraintArray, matrixRow, &skeletonMemory[skeletonMemorySizeInBytes]);
		skeletonMemorySizeInBytes += memorySizes[i];
	}


	const dgInt32 passes = world->m_solverMode;
	for (dgInt32 step = 0; step < maxPasses; step++) {
		if (joindDesc.m_firstPassCoefFlag == dgFloat32(0.0f)) {
			for (dgInt32 curJoint = 0; curJoint < jointCount; curJoint++) {
				dgJointInfo* const jointInfo = &constraintArray[curJoint];
				dgConstraint* const constraint = jointInfo->m_joint;
				if (constraint->m_solverActive) {
					joindDesc.m_rowsCount = jointInfo->m_pairCount;
					joindDesc.m_rowMatrix = &matrixRow[jointInfo->m_pairStart];
					constraint->JointAccelerations(&joindDesc);
				}
			}
			joindDesc.m_firstPassCoefFlag = dgFloat32(1.0f);
		} else {
			for (dgInt32 curJoint = 0; curJoint < jointCount; curJoint++) {
				dgJointInfo* const jointInfo = &constraintArray[curJoint];
				dgConstraint* const constraint = jointInfo->m_joint;
				if (constraint->m_solverActive) {
					const dgInt32 m0 = jointInfo->m_m0;
					const dgInt32 m1 = jointInfo->m_m1;
					const dgBody* const body0 = bodyArray[m0].m_body;
					const dgBody* const body1 = bodyArray[m1].m_body;
					if (!(body0->m_resting & body1->m_resting)) {
						joindDesc.m_rowsCount = jointInfo->m_pairCount;
						joindDesc.m_rowMatrix = &matrixRow[jointInfo->m_pairStart];
						constraint->JointAccelerations(&joindDesc);
					}
				}
			}
		}

		dgFloat32 accNorm(maxAccNorm * dgFloat32(2.0f));
		for (dgInt32 k = 0; (k < passes) && (accNorm > maxAccNorm); k++) {
			accNorm = dgFloat32(0.0f);
			for (dgInt32 i = 0; i < jointCount; i++) {
				dgJointInfo* const jointInfo = &constraintArray[i];
				dgFloat32 accel = CalculateJointForce(jointInfo, bodyArray, internalForces, matrixRow);
				accNorm = (accel > accNorm) ? accel : accNorm;
			}
		}

		for (dgInt32 i = 0; i < skeletonCount; i++) {
			skeletonArray[i]->CalculateJointForce(constraintArray, bodyArray, internalForces, matrixRow);
		}

		if (timestepRK != dgFloat32(0.0f)) {
			dgVector timestep4(timestepRK);
			for (dgInt32 i = 1; i < bodyCount; i++) {
				dgDynamicBody* const body = (dgDynamicBody*)bodyArray[i].m_body;
				dgAssert(body->m_index == i);
				ApplyNetVelcAndOmega(body, internalForces[i], timestep4, speedFreeze2, forceActiveMask);
				dgAssert(body->m_index == i);
			}
		} else {
			for (dgInt32 i = 1; i < bodyCount; i++) {
				dgBody* const body = bodyArray[i].m_body;
				if (body->m_active) {
					const dgVector& linearMomentum = internalForces[i].m_linear;
					const dgVector& angularMomentum = internalForces[i].m_angular;

					body->m_veloc += linearMomentum.Scale4(body->m_invMass.m_w);
					body->m_omega += body->m_invWorldInertiaMatrix.RotateVector(angularMomentum);
				}
			}
		}
	}

	dgInt32 hasJointFeeback = 0;
	if (timestepRK != dgFloat32(0.0f)) {
		for (dgInt32 i = 0; i < jointCount; i++) {
			dgJointInfo* const jointInfo = &constraintArray[i];
			dgConstraint* const constraint = jointInfo->m_joint;

			//const dgInt32 m0 = jointInfo->m_m0;
			//const dgInt32 m1 = jointInfo->m_m1;
			//dgBody* const body0 = bodyArray[m0].m_body;
			//dgBody* const body1 = bodyArray[m1].m_body;
			//body0->m_solverEquilibrium &= true;
			//body1->m_solverEquilibrium &= true;

			if (constraint->m_solverActive) {
				const dgInt32 first = jointInfo->m_pairStart;
				const dgInt32 count = jointInfo->m_pairCount;

				for (dgInt32 j = 0; j < count; j++) {
					dgJacobianMatrixElement* const row = &matrixRow[j + first];
					dgFloat32 val = row->m_force;
					dgAssert(dgCheckFloat(val));
					row->m_jointFeebackForce[0].m_force = val;
					row->m_jointFeebackForce[0].m_impact = row->m_maxImpact * timestepRK;
				}
				hasJointFeeback |= (constraint->m_updaFeedbackCallback ? 1 : 0);
			}
		}


		const dgVector invTime(invTimestep);
		const dgVector maxAccNorm2(maxAccNorm * maxAccNorm);
		for (dgInt32 i = 1; i < bodyCount; i++) {
			dgDynamicBody* const body = (dgDynamicBody*)bodyArray[i].m_body;
			ApplyNetTorqueAndForce(body, invTime, maxAccNorm2, forceActiveMask);
		}
		if (hasJointFeeback) {
			for (dgInt32 i = 0; i < jointCount; i++) {
				if (constraintArray[i].m_joint->m_updaFeedbackCallback) {
					constraintArray[i].m_joint->m_updaFeedbackCallback(*constraintArray[i].m_joint, timestep, threadIndex);
				}
			}
		}
	} else {
		for (dgInt32 i = 1; i < bodyCount; i++) {
			dgBody* const body = bodyArray[i].m_body;
			if (body->m_active) {
				body->m_netForce = dgVector::m_zero;
				body->m_netTorque = dgVector::m_zero;
			}
		}
	}
}


